#include "PrimeBridge.hpp"
#include "version.h"
#include <IOKit/IOLib.h>
#include <IOKit/IODeviceMemory.h>
#include <libkern/c++/OSDictionary.h>
#include <libkern/c++/OSIterator.h>
#include <libkern/sysctl.h>
#include <pexpert/pexpert.h>
#include <string.h>
#include <sys/vnode.h>
#include <sys/uio.h>
#include <sys/fcntl.h>
#include <sys/time.h>
#include <sys/kauth.h>
#include <sys/proc.h>

#define PBLOG(fmt, ...) do { IOLog("PrimeBridge: " fmt "\n", ##__VA_ARGS__); } while (0)

OSDefineMetaClassAndAbstractStructors(PrimeFrameSource, OSObject)
OSDefineMetaClassAndStructors(PrimeBridge, IOService)

// 680M (Rembrandt: GC 10.3.3, DCN 3.1.2) register byte offsets in BAR5, from Linux amdgpu:
// yellow_carp_offset.h (GC_BASE seg0 0x1260, DCN_BASE seg2 0x34C0), gc_10_3_0_offset.h, dcn_3_1_2_offset.h.
static constexpr uint32_t kRegFbOffset   = (0x1260 + 0x16e7) * 4;  // GCMC_VM_FB_OFFSET[23:0] << 24 = VRAM's system address
static constexpr uint32_t kRegFbLocBase  = (0x1260 + 0x16fc) * 4;  // GCMC_VM_FB_LOCATION_BASE[23:0]
static constexpr uint32_t kRegFbLocTop   = (0x1260 + 0x16fd) * 4;  // GCMC_VM_FB_LOCATION_TOP[23:0]
static constexpr uint32_t kRegOtg0Ctl    = (0x34C0 + 0x1b41) * 4;  // OTG_CONTROL, bit 0 = OTG_MASTER_EN
static constexpr uint32_t kRegOtg0HTotal = (0x34C0 + 0x1b2a) * 4;  // OTG_H_TOTAL[14:0] = h_total - 1
static constexpr uint32_t kRegOtg0VTotal = (0x34C0 + 0x1b2f) * 4;  // OTG_V_TOTAL[14:0] = v_total - 1
static constexpr uint32_t kRegOtg0Frames = (0x34C0 + 0x1b4c) * 4;  // OTG_STATUS_FRAME_COUNT[23:0]
static constexpr uint32_t kOtgStride     = 0x80 * 4;               // OTG1..3 follow every 0x80 registers

extern "C" int rdmsr_carefully(uint32_t msr, uint32_t *lo, uint32_t *hi);   // com.apple.kpi.unsupported

// Refresh rate in 1/100 Hz from two reads of the 24-bit OTG frame counter taken ns apart.
static constexpr uint32_t hz100(uint32_t f0, uint32_t f1, uint64_t ns) {
    return ns ? (uint32_t)((uint64_t)((f1 - f0) & 0xFFFFFFu) * 100000000000ULL / ns) : 0;
}
static_assert(hz100(0xFFFFF0u, 0x0006B0u, 12000000000ULL) == 14400, "1728 frames in 12 s across the wrap = 144.00 Hz");

// ---------------------------------------------------------------------------
// On-disk log sink. Every line is BOTH sent to IOLog (kernel ring buffer) AND
// appended to an in-RAM ring buffer that the tick loop flushes to a text file.
// The file target is /Volumes/1401/PrimeBridge.log (the USB key), which is not
// mounted when the kext starts; logFlush() opens it lazily with append and,
// until it succeeds, keeps lines buffered in RAM. The ring buffer drops the
// oldest lines if the flush cannot keep up, so a stuck mount can never panic
// or unboundedly grow the kernel heap.
// ---------------------------------------------------------------------------

void PrimeBridge::logf(const char *fmt, ...) {
    if (!logEnabled_) return;

    // Always mirror to the kernel log first (even if the ring buffer is not set up).
    {
        va_list va;
        va_start(va, fmt);
        char line[512];
        vsnprintf(line, sizeof(line), fmt, va);
        va_end(va);
        IOLog("PrimeBridge: %s\n", line);
    }

    if (!logBuf_ || logBufCap_ == 0) return;

    struct timeval tv;
    microuptime(&tv);

    char line[768];
    va_list va;
    va_start(va, fmt);
    int n = vsnprintf(line, sizeof(line), fmt, va);
    va_end(va);
    if (n < 0) return;
    if ((size_t)n >= sizeof(line)) n = (int)sizeof(line) - 1;

    // "[<sec>.<usec>] <line>\n"
    char stamped[900];
    int sn = snprintf(stamped, sizeof(stamped), "[%lld.%06lld] %s\n",
                      (long long)tv.tv_sec, (long long)tv.tv_usec, line);
    if (sn < 0) return;
    const size_t want = (size_t)sn < sizeof(stamped) ? (size_t)sn : sizeof(stamped) - 1;

    if (want > logBufCap_) return; // a single line larger than the buffer is dropped

    // Protect the ring buffer against concurrent logf() from the timer workloop
    // and registerFrameSource().
    if (logLock_) IOLockLock(logLock_);

    // Drop oldest lines until this one fits.
    while (logBufLen_ + want > logBufCap_) {
        uint8_t *nl = (uint8_t *)memchr(logBuf_, '\n', logBufLen_);
        if (!nl) { logBufLen_ = 0; break; }
        size_t drop = (size_t)(nl - logBuf_) + 1;
        memmove(logBuf_, logBuf_ + drop, logBufLen_ - drop);
        logBufLen_ -= drop;
    }
    memcpy(logBuf_ + logBufLen_, stamped, want);
    logBufLen_ += want;

    if (logLock_) IOLockUnlock(logLock_);
}

void PrimeBridge::logFlush() {
    if (!logEnabled_ || !logBuf_ || logBufLen_ == 0) return;

    // Candidate targets in priority order. macOS lives on the internal disk, so
    // /var/log is writable once the Data volume is up; the USB key's "1401" volume
    // is NOT mounted under macOS (it only mounts when plugged into the build host),
    // so it is a fallback rather than the primary target.
    static const char *const kCandidates[] = {
        "/var/log/PrimeBridge.log",
        "/Volumes/1401/PrimeBridge.log",
    };

    // Choose the first target that opens for append and remember it.
    if (!logReady_) {
        for (size_t i = 0; i < sizeof(kCandidates) / sizeof(kCandidates[0]); i++) {
            vfs_context_t c = vfs_context_create(nullptr);
            if (!c) continue;
            vnode_t vp = nullptr;
            errno_t err = vnode_open(kCandidates[i], FWRITE | O_APPEND | O_CREAT, 0644, 0, &vp, c);
            if (err == 0 && vp) {
                vnode_close(vp, FWRITE, c);
                snprintf(logPath_, sizeof(logPath_), "%s", kCandidates[i]);
                logReady_ = true;
            }
            vfs_context_rele(c);
            if (logReady_) break;
        }
        if (!logReady_) return; // no writable target yet; retry on a later tick
    }

    vfs_context_t ctx = vfs_context_create(nullptr);
    if (!ctx) return;

    vnode_t vp = nullptr;
    int fmode = FWRITE | O_APPEND | O_CREAT;
    errno_t err = vnode_open(logPath_, fmode, 0644, 0, &vp, ctx);
    if (err != 0 || !vp) {
        vfs_context_rele(ctx);
        return; // target became unwritable; buffer retained
    }

    const size_t n = logBufLen_;
    int resid = 0;
    int wr = vn_rdwr(UIO_WRITE, vp, (caddr_t)logBuf_, (int)n, 0,
                     UIO_SYSSPACE, IO_APPEND | IO_SYNC, vfs_context_ucred(ctx), &resid, kernproc);

    vnode_close(vp, FWRITE, ctx);
    vfs_context_rele(ctx);

    if (wr == 0) {
        logBufLen_ = 0;
    }
}

void PrimeBridge::logShutdown() {
    // Final best-effort flush before the object is torn down.
    logFlush();
    if (logBuf_) { IOFree(logBuf_, logBufCap_); logBuf_ = nullptr; }
    logBufCap_ = 0;
    logBufLen_ = 0;
}

IOService *PrimeBridge::probe(IOService *provider, SInt32 *score) {
    int dummy = 0;
    if (PE_parse_boot_argn("-primeoff", &dummy, sizeof(dummy))) return nullptr;  // escape hatch
    return IOService::probe(provider, score);
}

bool PrimeBridge::start(IOService *provider) {
    if (!IOService::start(provider)) return false;
    if (!setup(provider)) { cleanup(); IOService::stop(provider); return false; }
    registerService();
    return true;
}

bool PrimeBridge::setup(IOService *provider) {
    igpu_ = OSDynamicCast(IOPCIDevice, provider);
    if (!igpu_) return false;
    igpu_->retain();

    // Allocate the log ring buffer (256 KB) before any other step so failures are logged.
    int dummy = 0;
    if (PE_parse_boot_argn("-primelogoff", &dummy, sizeof(dummy))) {
        // A boot-arg beginning with "-" marks itself merely by existing; the
        // parser never writes the value the caller expects. Treat presence as
        // "off", not "1".
        logEnabled_ = false;
    }
    if (logEnabled_) {
        logBufCap_ = 256 * 1024;
        logBuf_ = static_cast<uint8_t *>(IOMalloc(logBufCap_));
        if (logBuf_) logBufLen_ = 0;
    }
    logf("PrimeBridge " PRIMEBRIDGE_VERSION " starting (log=%d)", logEnabled_ ? 1 : 0);

    if (!fb_.attach(igpu_)) { logf("firmware framebuffer not found or invalid"); return false; }

    // Single rich diagnostic line, all integer (kernel printf has no %f).
    // stride is v_rowBytes octets. stride_px = stride/4 (bpp=32). padding_px =
    // stride_px - width; non-zero means the firmware padded the scanline.
    {
        const uint32_t stridePx = fb_.stride() / 4u;
        logf("FB %ux%u bpp %u stride %u B stride_px %u padding_px %d phys 0x%llx flags 0x%llx BAR0 0x%llx +0x%llx off_in_bar0 0x%llx",
             fb_.width(), fb_.height(), fb_.depth(), fb_.stride(),
             stridePx, (int)stridePx - (int)fb_.width(),
             fb_.physAddr(), fb_.flagBits(), fb_.bar0Phys(), fb_.bar0Len(),
             fb_.physAddr() - fb_.bar0Phys());
    }

    // Registry diagnostics (always readable via: ioreg -l -w0 -r -c PrimeBridge)
    setProperty("GOPWidth",  fb_.width(),  32);
    setProperty("GOPHeight", fb_.height(), 32);
    setProperty("GOPStride", fb_.stride(), 32);
    setProperty("GOPStridePixels", (uint64_t)(fb_.stride() / 4u), 64);
    setProperty("GOPPaddingPixels", (uint64_t)((fb_.stride() / 4u > fb_.width()) ? (fb_.stride() / 4u - fb_.width()) : 0), 64);
    setProperty("GOPDepth",  fb_.depth(),  32);
    setProperty("GOPPhys",   fb_.physAddr(), 64);
    setProperty("GOPFlagBits", fb_.flagBits(), 64);
    setProperty("BAR0Phys",  fb_.bar0Phys(), 64);
    setProperty("BAR0Len",   fb_.bar0Len(),  64);
    setProperty("PrimeBridgeVersion", PRIMEBRIDGE_VERSION);
    setProperty("PrimeMode", "boot-waiting-nvfb");

    symScanoutInfo_ = OSSymbol::withCStringNoCopy("nvScanoutInfo");

    // Optional boot-args:
    // primetest=1 : SMPTE bars during boot (until NVRMFB is ready)
    // primetest=2 : diagnostic pattern during boot (until NVRMFB is ready)
    // primetest=3 : hold diagnostic pattern for primehold seconds even after NVRMFB is ready
    // primefmt=1  : 8-bit BGRX8888, byte3 forced 0 (DEFAULT; normal GOP/macOS order)
    // primefmt=2  : 8-bit RGBX8888, byte3 forced 0 (R/B swapped)
    // primefmt=3  : raw 32BGRA passthrough (v0.2.0)
    // primefmt=0  : legacy "Mirrored Blue" (v0.3.1)
    // primefmt=4  : comparison-only; "calibrated" against a 1-byte-shifted address
    PE_parse_boot_argn("primetest",   &testMode_,       sizeof(testMode_));
    PE_parse_boot_argn("primemirror", &mirrorExternal_, sizeof(mirrorExternal_));
    PE_parse_boot_argn("primedirect", &directVram_,     sizeof(directVram_));
    PE_parse_boot_argn("primefmt",    &pixelFmt_,       sizeof(pixelFmt_));
    if (pixelFmt_ > 4) pixelFmt_ = 4;
    setProperty("PixelFormat", pixelFmt_, 32);

    // Calibration mire: primecolors=1 cycles raw single-byte probes + checkerboard
    // so the DCN crossbar and stride are measured independently of any packing.
    PE_parse_boot_argn("primecolors", &colorTest_, sizeof(colorTest_));
    if (colorTest_ == 1) {
        colorStage_ = 0;
        colorStageTicks_ = 0;
        fb_.drawCalibration(0);
        logf("calibration mire: stage 0 (byte0)");
    }
    setProperty("ColorTest", colorTest_, 32);

    uint32_t holdS = 60;
    PE_parse_boot_argn("primehold", &holdS, sizeof(holdS));
    if (holdS > 600) holdS = 600;
    if (testMode_) {
        fb_.drawTestPattern(testMode_ == 3 ? 2 : testMode_, pixelFmt_);
        holdTicks_ = holdS * 4;  // 250 ms ticks during boot
        logf("test pattern %u drawn (fmt %u), hold %u s during boot", testMode_, pixelFmt_, holdS);
    }
    setProperty("TestMode", testMode_, 32);

    uint32_t hz = 60;   // default 60 Hz (the internal panel refreshes at 60 Hz)
    PE_parse_boot_argn("primehz", &hz, sizeof(hz));
    if (hz < 24 || hz > 240) hz = 60;
    targetPeriodUs_ = periodUs_ = 1000000 / hz;
    setProperty("TargetHz", hz, 32);

    lock_  = IOLockAlloc();
    logLock_ = IOLockAlloc();
    wl_    = IOWorkLoop::workLoop();
    timer_ = IOTimerEventSource::timerEventSource(this,
               OSMemberFunctionCast(IOTimerEventSource::Action, this, &PrimeBridge::copyTick));
    if (!(lock_ && logLock_ && wl_ && timer_ && wl_->addEventSource(timer_) == kIOReturnSuccess)) return false;

    // Poll every 250 ms during boot until NVRMNVDAFramebuffer + NVAccel are armed
    timer_->setTimeoutMS(250);
    return true;
}

bool PrimeBridge::registerFrameSource(PrimeFrameSource *src) {
    if (!src || !lock_) return false;
    IOLockLock(lock_);
    if (source_) { IOLockUnlock(lock_); return false; }
    src->retain(); source_ = src; lastSeq_ = 0; holdTicks_ = 0;
    IOLockUnlock(lock_);
    timer_->setTimeoutUS(periodUs_);
    setProperty("PrimeMode", "explicit-frame-source");
    logf("frame source registered");
    return true;
}

void PrimeBridge::unregisterFrameSource(PrimeFrameSource *src) {
    IOLockLock(lock_);
    if (source_ == src) { source_->release(); source_ = nullptr; }
    IOLockUnlock(lock_);
}

bool PrimeBridge::findNvidiaFramebuffer() {
    if (nvfb_) return true;
    if (!symScanoutInfo_) return false;

    OSDictionary *match = IOService::serviceMatching("NVRMNVDAFramebuffer");
    if (!match) return false;
    OSIterator *iter = IOService::getMatchingServices(match);
    match->release();
    if (!iter) return false;

    bool found = false;
    while (OSObject *obj = iter->getNextObject()) {
        IOService *cand = OSDynamicCast(IOService, obj);
        if (!cand) continue;

        NVRMVramRequest req {};
        req.version = 2;
        IOReturn rc = cand->callPlatformFunction(symScanoutInfo_, false, &req, nullptr, nullptr, nullptr);
        if (rc == kIOReturnSuccess && req.kva != nullptr &&
            req.width >= 640 && req.height >= 480 && req.pitch >= req.width * 4) {
            cand->retain();
            nvfb_     = cand;
            nvWidth_  = req.width;
            nvHeight_ = req.height;
            nvPitch_  = req.pitch;
            nvPhys_   = req.phys;
            nvSize_   = req.actualSize;

            setProperty("NVFBFound",  true);
            setProperty("NVFBWidth",  nvWidth_,  32);
            setProperty("NVFBHeight", nvHeight_, 32);
            setProperty("NVFBPitch",  nvPitch_,  32);
            setProperty("NVFBPhys",   nvPhys_,   64);
            setProperty("NVFBSize",   nvSize_,   64);
            setProperty("PrimeMode",  "nvfb-found-waiting-iop");
            logf("found NVRMNVDAFramebuffer %ux%u pitch %u phys 0x%llx",
                  nvWidth_, nvHeight_, nvPitch_, nvPhys_);
            found = true;
            break;
        }
    }
    iter->release();
    return found;
}

bool PrimeBridge::attachNvidiaScanout() {
    if (!nvfb_ || !symScanoutInfo_) return false;

    NVRMVramRequest req {};
    req.version = 2;
    IOReturn rc = nvfb_->callPlatformFunction(symScanoutInfo_, false, &req, nullptr, nullptr, nullptr);
    if (rc != kIOReturnSuccess || !req.kva || req.width < 640 || req.height < 480 || req.pitch < req.width * 4) {
        return false;
    }

    nvWidth_   = req.width;
    nvHeight_  = req.height;
    nvPitch_   = req.pitch;
    nvPhys_    = req.phys;
    nvSize_    = req.actualSize;
    origNvKva_ = static_cast<uint8_t *>(req.kva);

    // Dynamically verify NVRMNVDAFramebuffer's internal field offsets against req before touching 0x2d8.
    // Offsets confirmed by disassembly of the shipped 2.6.3 binary:
    //   0x2d8 = fKva, 0x2e0 = fPhys, 0x2f0 = fPitch, 0x2f4 = fW, 0x2f8 = fH.
    const uint8_t *raw   = reinterpret_cast<const uint8_t *>(nvfb_);
    void    *kvaAt2d8    = *reinterpret_cast<void * const volatile *>(raw + 0x2d8);
    uint64_t physAt2e0   = *reinterpret_cast<const volatile uint64_t *>(raw + 0x2e0);
    uint32_t pitchAt2f0  = *reinterpret_cast<const volatile uint32_t *>(raw + 0x2f0);
    uint32_t widthAt2f4  = *reinterpret_cast<const volatile uint32_t *>(raw + 0x2f4);
    uint32_t heightAt2f8 = *reinterpret_cast<const volatile uint32_t *>(raw + 0x2f8);

    offsetsVerified_ = (kvaAt2d8 == req.kva &&
                        physAt2e0 == req.phys &&
                        pitchAt2f0 == req.pitch &&
                        widthAt2f4 == req.width &&
                        heightAt2f8 == req.height);
    setProperty("NVFBOffsetsVerified", offsetsVerified_);

    if (offsetsVerified_ && directVram_ == 0) {
        // Never shrink: NVRMFB's switchMode keeps fKva (= staging_), so after a mode change NVAccel
        // writes the new mode's frames here before tickNullMoth's geometry check sees it.
        // shortcut: a mode larger than the first one attached still overflows until that check
        // (only nvfbhidpi=1 or a bigger external monitor allow one), upgrade: size from NVRMFB's
        // scanout allocation (fMemBytes, not in the nvScanoutInfo ABI).
        const size_t need = (size_t)nvPitch_ * nvHeight_;
        if (!staging_ || need > stagingSize_) {
            // 0x2d8 already points back at VRAM here, so the old buffers are unreferenced.
            if (staging_) { IOFreeAligned(staging_, stagingSize_); staging_ = nullptr; }
            if (shadow_)  { IOFreeAligned(shadow_, stagingSize_);  shadow_  = nullptr; }
            stagingSize_ = need;
            staging_ = static_cast<uint8_t *>(IOMallocAligned(stagingSize_, PAGE_SIZE));
            shadow_  = static_cast<uint8_t *>(IOMallocAligned(stagingSize_, PAGE_SIZE));
        }
        if (staging_) {
            memcpy(staging_, origNvKva_, need);
            if (shadow_) memset(shadow_, 0, need);
            // Redirect NVRMNVDAFramebuffer's fKva (0x2d8) to cached system RAM staging_, so
            // NVAccel's performTransaction deposits frames into cached RAM instead of VRAM.
            __asm__ volatile("sfence" ::: "memory");
            *reinterpret_cast<void * volatile *>(reinterpret_cast<uint8_t *>(nvfb_) + 0x2d8) = staging_;
            __asm__ volatile("sfence" ::: "memory");
            setProperty("PrimeMode", "staging-redirect-ce-8bit");
            logf("redirected NVRMNVDAFramebuffer scanout kva to cached staging (%llu bytes)",
                  (unsigned long long)stagingSize_);
        } else {
            setProperty("PrimeMode", "direct-vram-alloc-failed");
        }
    } else {
        setProperty("PrimeMode", "direct-vram-fallback");
    }

    redirected_ = true;
    firstFrame_ = true;
    return true;
}

// Read-only groundwork for writing the 680M's memory directly instead of through BAR0, as Linux amdgpu
// does on x86 APUs (gmc_v10_0.c): where that memory sits in system RAM, whether BAR0 and that address
// show the same pixels, and the panel timing. Never writes; runs once, on a still screen.
void PrimeBridge::probeIgpu() {
    IODeviceMemory *bar5 = igpu_->getDeviceMemoryWithRegister(kIOPCIConfigBaseAddress5);
    if (!mmio_ && bar5 && bar5->getLength() >= 0x80000)
        mmio_ = bar5->createMappingInTask(kernel_task, 0, kIOMapAnywhere | kIOMapInhibitCache, 0, 0x80000);
    if (!mmio_) { logf("igpu probe: BAR5 not mapped"); return; }
    regs_ = reinterpret_cast<volatile uint32_t *>(mmio_->getVirtualAddress());

    const uint32_t off = regs_[kRegFbOffset / 4], base = regs_[kRegFbLocBase / 4], top = regs_[kRegFbLocTop / 4];
    const uint64_t vramPhys = (uint64_t)(off & 0xFFFFFFu) << 24;
    const uint64_t vramSize = (top & 0xFFFFFFu) >= (base & 0xFFFFFFu)
                            ? (uint64_t)((top & 0xFFFFFFu) - (base & 0xFFFFFFu) + 1) << 24 : 0;
    const uint64_t gopOff = fb_.physAddr() - fb_.bar0Phys(), len = (uint64_t)fb_.stride() * fb_.height();
    logf("igpu probe: FB_OFFSET 0x%08x FB_LOCATION 0x%08x..0x%08x -> VRAM at phys 0x%llx, %llu MB; GOP at VRAM +0x%llx",
         off, base, top, vramPhys, vramSize >> 20, gopOff);

    IOMemoryMap *ram = nullptr;
    if (off != 0xFFFFFFFFu && vramPhys && gopOff + len <= vramSize) {
        // Default cache mode: uncached if this is reserved memory as expected, the kernel's own mode if not.
        if (IODeviceMemory *md = IODeviceMemory::withRange(vramPhys + gopOff, len)) {
            ram = md->createMappingInTask(kernel_task, 0, kIOMapAnywhere | kIOMapDefaultCache | kIOMapReadOnly);
            md->release();
        }
    }
    // What PrimeBridge last wrote (shadow_, packed like blitRow's default format) is the reference: BAR0 reads
    // are only logged, since the 0.4.6 probe read all-black rows through BAR0 on a lit desktop.
    const bool haveRef = shadow_ && pixelFmt_ == 1 && nvPitch_ == fb_.stride() && nvHeight_ >= fb_.height();
    if (ram && haveRef) {
        uint32_t px = 0, diffBar = 0, diffRef = 0, litBar = 0, litRef = 0;
        for (uint32_t k = 0; k < 32; k++) {
            const uint64_t row = (uint64_t)((2 * k + 1) * fb_.height() / 64) * fb_.stride();
            const volatile uint32_t *a = reinterpret_cast<const volatile uint32_t *>(fb_.base() + row);
            const volatile uint32_t *b = reinterpret_cast<const volatile uint32_t *>(ram->getVirtualAddress() + row);
            const uint32_t *s = reinterpret_cast<const uint32_t *>(shadow_ + row);
            for (uint32_t x = 0; x < fb_.width(); x++, px++) {
                const uint32_t pa = a[x], pb = b[x], ps = s[x] & 0x00FFFFFFu;
                if (pa != pb) diffBar++;
                if (ps != pb) diffRef++;
                if (pa & 0xFFFFFFu) litBar++;
                if (ps) litRef++;
            }
        }
        ram->release();
        const bool match = !diffRef && litRef >= 1000;
        logf("igpu probe: phys 0x%llx, %u px: %u differ from what we wrote (%u lit), %u from BAR0 (%u lit) -> %s",
             vramPhys + gopOff, px, diffRef, litRef, diffBar, litBar,
             diffRef ? "NO MATCH" : match ? "MATCH" : "inconclusive (screen too dark)");
        if (diffRef || match) probed_ = true;   // a dark screen is tried again on a later still screen
        uint32_t carve = 1;
        PE_parse_boot_argn("primecarve", &carve, sizeof(carve));
        if (match && carve) chooseWriteMapping(vramPhys + gopOff);
    } else {
        if (ram) ram->release();
        probed_ = true;
        logf("igpu probe: no comparison (VRAM address implausible or not mappable, or no reference frame)");
    }

    // Panel timing: the OTG the GOP left running drives the panel.
    for (uint32_t i = 0; i < 4 && !otgFrameReg_; i++) {
        if (!(regs_[(kRegOtg0Ctl + i * kOtgStride) / 4] & 1u)) continue;
        otgFrameReg_ = kRegOtg0Frames + i * kOtgStride;
        otgFrames_ = regs_[otgFrameReg_ / 4];
        otgAbs_ = mach_absolute_time();
        logf("igpu probe: OTG%u running, h_total %u v_total %u", i,
             (regs_[(kRegOtg0HTotal + i * kOtgStride) / 4] & 0x7FFFu) + 1,
             (regs_[(kRegOtg0VTotal + i * kOtgStride) / 4] & 0x7FFFu) + 1);
    }
    if (!otgFrameReg_) logf("igpu probe: no OTG running");
}

// Times rewriting 64 rows with the pixels already on screen (invisible) through BAR0 as attach() mapped it,
// BAR0 write-combined, and the 680M's VRAM write-combined, then keeps the fastest. Runs on a still tick.
void PrimeBridge::chooseWriteMapping(uint64_t vramGop) {
    uint32_t lo = 0, hi = 0;
    if (rdmsr_carefully(0x277, &lo, &hi) == 0)   // IA32_PAT: XNU's write-combined PTEs use entry PA6
        logf("igpu writes: PAT 0x%08x%08x (PA6 %s)", hi, lo, ((hi >> 16) & 7u) == 1 ? "WC" : "not WC");
    auto bench = [this]() -> uint64_t {
        const uint64_t t0 = mach_absolute_time();
        for (uint32_t k = 0; k < 64; k++) {
            const uint32_t y = (2 * k + 1) * fb_.height() / 128;
            fb_.blitRow(y, reinterpret_cast<const uint32_t *>(shadow_ + (uint64_t)y * nvPitch_), fb_.width(), pixelFmt_);
        }
        __builtin_ia32_sfence();
        uint64_t ns;
        absolutetime_to_nanoseconds(mach_absolute_time() - t0, &ns);
        return ns / 64;
    };
    const uint64_t tBar = bench();
    const uint64_t tBarWC = fb_.mapWriteCombined(fb_.physAddr()) ? bench() : ~0ULL;
    const uint64_t tVram = fb_.mapWriteCombined(vramGop) ? bench() : ~0ULL;
    const bool vram = tVram < tBarWC && tVram < tBar;
    if (!vram) fb_.mapWriteCombined(fb_.physAddr());   // never worse than attach()'s mapping: both at worst uncached
    const uint64_t best = vram ? tVram : (tBarWC < tBar ? tBarWC : tBar);
    fastWrites_ = best < 3000;   // >= 2.5 GB/s: only then can NVAccel afford to write every frame to the panel
    setProperty("PrimeMode", vram ? "staging-redirect-carveout" : "staging-redirect-ce-8bit");
    logf("igpu writes: ns per row: BAR0 %llu, BAR0 write-combined %llu, VRAM write-combined %llu -> %s%s",
         tBar, tBarWC, tVram, vram ? "VRAM at phys" : "BAR0", fastWrites_ ? ", fast" : "");
}

void PrimeBridge::tickNullMoth(IOTimerEventSource *t) {
    // Calibration mire (primecolors=1): test the iGPU scanout ALONE. Cycle the 6
    // raw-probe stages at ~2 s each and never run frame copy, so the DCN crossbar
    // and stride are measured with zero interference from NVRMFB or packing. This
    // works with or without an external monitor (GOP is always scanned by the DCN).
    if (colorTest_ == 1) {
        if (++colorStageTicks_ >= 8) {   // ~250 ms tick => ~2 s per stage
            colorStageTicks_ = 0;
            colorStage_ = (colorStage_ + 1) % 6;
            fb_.drawCalibration(colorStage_);
            logf("calibration mire: stage %u", colorStage_);
        }
        if ((++logTick_ & 15u) == 0) logFlush();
        t->setTimeoutMS(250);
        return;
    }

    // Flush the log on a cadence independent of the frame-copy statTick_, so the
    // boot-waiting and iop-arming phases (which may last tens of seconds) are
    // persisted too, not just the post-redirect steady state.
    if ((++logTick_ & 15u) == 0) logFlush();

    if (!nvfb_) {
        findNvidiaFramebuffer();
        if (!nvfb_) {
            if (testMode_ && holdTicks_ > 0) {
                holdTicks_--;
                fb_.drawTestPattern(testMode_ == 3 ? 2 : testMode_, pixelFmt_);
            }
            t->setTimeoutMS(250);
            return;
        }
    }

    if (!redirected_) {
        // Wait until NVRM::autoGo has armed NVAccel's display pipe (debug.nvaccel_iop == 1)
        int iopArmed = 0;
        size_t iopSz = sizeof(iopArmed);
        sysctlbyname("debug.nvaccel_iop", &iopArmed, &iopSz, nullptr, 0);

        if (iopArmed != 1 || (testMode_ == 3 && holdTicks_ > 0)) {
            if (testMode_ && holdTicks_ > 0) {
                holdTicks_--;
                fb_.drawTestPattern(testMode_ == 3 ? 2 : testMode_, pixelFmt_);
            }
            t->setTimeoutMS(200);
            return;
        }

        // Ensure debug.nvaccel_iop_flip = 0 so NVAccel uses the copy path into staging_.
        // (Zero-copy flip is incompatible with the kva redirect — see nullmoth-internals.)
        int flipVal = 0;
        size_t flipSz = sizeof(flipVal);
        if (sysctlbyname("debug.nvaccel_iop_flip", &flipVal, &flipSz, nullptr, 0) == 0 && flipVal != 0) {
            int zero = 0;
            sysctlbyname("debug.nvaccel_iop_flip", nullptr, nullptr, &zero, sizeof(zero));
            flipCleared_++;
            setProperty("PrimeFlipCleared", flipCleared_, 32);
        }

        if (!attachNvidiaScanout()) {
            t->setTimeoutMS(200);
            return;
        }
    }

    const uint8_t *srcBuf = staging_ ? staging_ : origNvKva_;
    if (!srcBuf) {
        t->setTimeoutMS(100);
        return;
    }

    // Re-verify the scanout geometry ~1/s to detect a mode change, which would
    // otherwise make the copy walk stale dimensions (and overflow if it grew).
    if ((statTick_ & 63u) == 0 && redirected_) {
        NVRMVramRequest req {};
        req.version = 2;
        if (nvfb_->callPlatformFunction(symScanoutInfo_, false, &req, nullptr, nullptr, nullptr) == kIOReturnSuccess) {
            const void *ours = directKva_ ? (const void *)fb_.base() : (const void *)(staging_ ? staging_ : origNvKva_);
            if (req.kva && (req.width != nvWidth_ || req.height != nvHeight_ || req.pitch != nvPitch_ ||
                            req.kva != ours)) {
                logf("scanout geometry changed (or kva no longer staging): %ux%u p%u -> %ux%u p%u; restoring orig kva and re-attaching",
                     nvWidth_, nvHeight_, nvPitch_, req.width, req.height, req.pitch);
                // Restore NVRMNVDAFramebuffer's fKva back to the original VRAM
                // pointer BEFORE declaring the redirect dead, otherwise the next
                // attachNvidiaScanout() verification sees the stale staging_ and
                // can never re-match (self-deadlock: frozen frame + reset loop).
                if (nvfb_ && origNvKva_ && offsetsVerified_) {
                    __asm__ volatile("sfence" ::: "memory");
                    *reinterpret_cast<void * volatile *>(reinterpret_cast<uint8_t *>(nvfb_) + 0x2d8) = origNvKva_;
                    __asm__ volatile("sfence" ::: "memory");
                }
                redirected_ = false;
                directKva_ = false;
                t->setTimeoutMS(200);
                return;   // re-attach on a later tick via the !redirected_ path
            }
        }
    }

    const uint32_t w = nvWidth_  < fb_.width()  ? nvWidth_  : fb_.width();
    const uint32_t h = nvHeight_ < fb_.height() ? nvHeight_ : fb_.height();
    const size_t   rowBytes = (size_t)w * 4;

    bool anyRowChanged = false;

    // Diff frames in cached system RAM on every tick (primehz, default 60 Hz).
    // NOTE: NVAccel's performTransaction writes staging_ on its own workloop while this
    // reads it — a torn frame can slip through. Tearing is inherent to the redirect;
    // acceptable vs. the uncached-VRAM alternative.
    const uint64_t workStart = mach_absolute_time();
    if (!directKva_) {   // direct: NVAccel already wrote the frame into the GOP
        uint8_t *mirrorDst = (staging_ && mirrorExternal_) ? origNvKva_ : nullptr;
        for (uint32_t y = 0; y < h; y++) {
            const uint32_t *srcRow = reinterpret_cast<const uint32_t *>(srcBuf + (uint64_t)y * nvPitch_);
            uint32_t       *shdRow = shadow_ ? reinterpret_cast<uint32_t *>(shadow_ + (uint64_t)y * nvPitch_) : nullptr;

            if (firstFrame_ || !shdRow || memcmp(srcRow, shdRow, rowBytes) != 0) {
                if (shdRow) memcpy(shdRow, srcRow, rowBytes);
                if (mirrorDst) memcpy(mirrorDst + (uint64_t)y * nvPitch_, srcRow, rowBytes);
                const uint64_t b = mach_absolute_time();
                fb_.blitRow(y, srcRow, w, pixelFmt_);
                statBlit_ += mach_absolute_time() - b;
                statRows_++;
                anyRowChanged = true;
            }
        }
        if (anyRowChanged) {
            firstFrame_ = false;
            framesCopied_++;
        }
    }

    if (anyRowChanged) __builtin_ia32_sfence();
    const uint64_t work = mach_absolute_time() - workStart;
    statWork_ += work;
    if (work > statWorkMax_) statWorkMax_ = work;

    // Adaptive frequency: when nothing changed, idleTicks_ climbs; after a short
    // idle the loop drops from the target rate to 20 Hz and only restores it when
    // a change is seen again. Keeps the CPU able to sleep when the desktop is still.
    if (anyRowChanged) {
        idleTicks_ = 0;
        if (lowFreqMode_) { lowFreqMode_ = false; }
    } else {
        idleTicks_++;
    }
    if (!lowFreqMode_ && idleTicks_ >= 30) lowFreqMode_ = true;

    // Update IORegistry diagnostics + flush the log file + log effective rate ~5 s.
    if (++statTick_ >= 720) {
        statTick_ = 0;
        setProperty("FramesCopied", framesCopied_, 64);
        setProperty("EffectiveRateHz", (uint64_t)(1000000 / (lowFreqMode_ ? 50000u : periodUs_)), 64);
        // Per 720 ticks: copy loop time (avg, max), of which writes to the GOP, and rows written per tick.
        uint64_t workNs, maxNs, blitNs;
        absolutetime_to_nanoseconds(statWork_, &workNs);
        absolutetime_to_nanoseconds(statWorkMax_, &maxNs);
        absolutetime_to_nanoseconds(statBlit_, &blitNs);
        // Panel refresh over the same window, from the 680M's own frame counter (found by probeIgpu).
        uint32_t panelHz100 = 0;
        if (otgFrameReg_) {
            const uint32_t f = regs_[otgFrameReg_ / 4];
            const uint64_t at = mach_absolute_time();
            uint64_t ns;
            absolutetime_to_nanoseconds(at - otgAbs_, &ns);
            panelHz100 = hz100(otgFrames_, f, ns);
            otgFrames_ = f;
            otgAbs_ = at;
        }
        logf("rate-hint: %u Hz%s, work avg %llu us max %llu us, GOP writes avg %llu us, rows/tick %llu, panel %u.%02u Hz%s",
             1000000 / (lowFreqMode_ ? 50000u : periodUs_), lowFreqMode_ ? " (idle)" : "",
             workNs / 720000, maxNs / 1000, blitNs / 720000, statRows_ / 720, panelHz100 / 100, panelHz100 % 100,
             directKva_ ? ", direct" : "");
        statWork_ = statWorkMax_ = statBlit_ = statRows_ = 0;
        if (!probed_ && !anyRowChanged) probeIgpu();   // still screen: no BAR0 write in flight
        // Once GOP writes are fast and the mode is the panel's, NVAccel writes each frame straight into the panel's
        // buffer: no staging copy, no diff, no second clock. Raw BGRA is fine: the DCN ignores byte 3 (blitRow zeroes it).
        if (fastWrites_ && !directKva_ && redirected_ && staging_ && offsetsVerified_ && pixelFmt_ == 1 && !mirrorExternal_ &&
            nvWidth_ == fb_.width() && nvHeight_ == fb_.height() && nvPitch_ == fb_.stride()) {
            __asm__ volatile("sfence" ::: "memory");
            *reinterpret_cast<void * volatile *>(reinterpret_cast<uint8_t *>(nvfb_) + 0x2d8) = fb_.base();
            __asm__ volatile("sfence" ::: "memory");
            directKva_ = true;
            setProperty("PrimeMode", "direct-kva");
            logf("NVAccel now writes its frames straight into the panel's buffer (no staging copy, no diff)");
        }
        logFlush();
    }

    // Schedule the next tick at the effective rate (20 Hz when idle) on a fixed cadence: a delay
    // counted from the end of the work made each tick last the period plus the work (52 Hz, not 60).
    uint64_t period, now = mach_absolute_time();
    clock_interval_to_absolutetime_interval(lowFreqMode_ ? 50000u : periodUs_, kMicrosecondScale, &period);
    nextTickAbs_ = nextTickAbs_ + period > now ? nextTickAbs_ + period : now + period;   // late: restart
    t->wakeAtTime(nextTickAbs_);
}

void PrimeBridge::copyTick(IOTimerEventSource *t) {
    IOLockLock(lock_);
    PrimeFrameSource *src = source_;
    if (src) src->retain();
    IOLockUnlock(lock_);

    if (!src) {
        tickNullMoth(t);
        return;
    }

    PrimeFrame f {};
    if (src->acquireFrame(&f)) {
        if (f.seq != lastSeq_) {
            if (src->gpuCopyTo(f, fb_.physAddr(), fb_.stride()) != kIOReturnSuccess)
                fb_.blitFromCPU(f, pixelFmt_);
            lastSeq_ = f.seq;
            framesCopied_++;
        }
        src->releaseFrame(&f);
    }
    src->release();
    t->setTimeoutUS(periodUs_);
}

void PrimeBridge::cleanup() {
    if (timer_) {
        timer_->cancelTimeout();
        if (wl_) wl_->removeEventSource(timer_);
        timer_->release(); timer_ = nullptr;
    }
    if (wl_) { wl_->release(); wl_ = nullptr; }

    // Restore original NVRMNVDAFramebuffer scanout pointer before freeing staging_.
    if (nvfb_ && staging_ && origNvKva_ && offsetsVerified_) {
        __asm__ volatile("sfence" ::: "memory");
        *reinterpret_cast<void * volatile *>(reinterpret_cast<uint8_t *>(nvfb_) + 0x2d8) = origNvKva_;
        __asm__ volatile("sfence" ::: "memory");
    }
    // NVAccel's performTransaction may still be mid-write into staging_ on its own
    // workloop. Restoring the pointer does not stop that. Wait one period before
    // freeing so an in-flight copy lands in the original VRAM, not freed memory.
    // (Hot unload remains inherently racy; this only narrows the window.)
    __asm__ volatile("sfence" ::: "memory");
    if (staging_) { IOSleep(50); __asm__ volatile("sfence" ::: "memory"); }
    if (staging_) { IOFreeAligned(staging_, stagingSize_); staging_ = nullptr; }
    if (shadow_)  { IOFreeAligned(shadow_, stagingSize_);  shadow_  = nullptr; }
    stagingSize_ = 0;

    if (nvfb_) { nvfb_->release(); nvfb_ = nullptr; }
    if (symScanoutInfo_) { symScanoutInfo_->release(); symScanoutInfo_ = nullptr; }

    if (lock_) {
        IOLockLock(lock_);
        if (source_) { source_->release(); source_ = nullptr; }
        IOLockUnlock(lock_);
        IOLockFree(lock_); lock_ = nullptr;
    }
    if (logLock_) { IOLockFree(logLock_); logLock_ = nullptr; }
    logShutdown();
    if (mmio_) { mmio_->release(); mmio_ = nullptr; regs_ = nullptr; otgFrameReg_ = 0; }
    fb_.detach();
    if (igpu_) { igpu_->release(); igpu_ = nullptr; }
}

void PrimeBridge::stop(IOService *provider) { cleanup(); IOService::stop(provider); }
