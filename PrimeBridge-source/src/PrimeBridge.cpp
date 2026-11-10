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

static constexpr uint32_t kRegFbOffset   = (0x1260 + 0x16e7) * 4;
static constexpr uint32_t kRegFbOffsetVega = (0x2000 + 0x096b) * 4;
static constexpr uint32_t kFbLocBase     = 0x15;
static constexpr uint32_t kFbLocTop      = 0x16;
static_assert(0x16fc - 0x16e7 == kFbLocBase && 0x16fd - 0x16e7 == kFbLocTop && 0x0980 - 0x096b == kFbLocBase && 0x0981 - 0x096b == kFbLocTop,
              "FB_LOCATION_BASE and TOP sit 0x15 and 0x16 registers after FB_OFFSET in gc_10_3_0 (Rembrandt) and in gc_9_0 (Vega)");
static constexpr uint32_t kRegOtg0Ctl    = (0x34C0 + 0x1b41) * 4;
static constexpr uint32_t kRegOtg0HTotal = (0x34C0 + 0x1b2a) * 4;
static constexpr uint32_t kRegOtg0VTotal = (0x34C0 + 0x1b2f) * 4;
static constexpr uint32_t kRegOtg0Frames = (0x34C0 + 0x1b4c) * 4;
static constexpr uint32_t kOtgStride     = 0x80 * 4;
static constexpr uint64_t kFastRowNs     = 3000;

enum : uint32_t { kIgpuOther, kIgpuVega, kIgpuRembrandt, kIgpuDcn31 };
static constexpr uint32_t igpuGen(uint16_t dev) {
    return dev == 0x15dd || dev == 0x15d8 || dev == 0x1636 || dev == 0x1638 || dev == 0x164c || dev == 0x15e7 ? kIgpuVega
         : dev == 0x1681 || dev == 0x164d ? kIgpuRembrandt
         : dev == 0x15bf || dev == 0x15c8 || dev == 0x1900 || dev == 0x1901 || dev == 0x164e || dev == 0x13c0 || dev == 0x1506 ? kIgpuDcn31
         : kIgpuOther;
}
static_assert(igpuGen(0x1681) == kIgpuRembrandt && igpuGen(0x1638) == kIgpuVega && igpuGen(0x15bf) == kIgpuDcn31 &&
              igpuGen(0x150e) == kIgpuOther && igpuGen(0x1114) == kIgpuOther && igpuGen(0x1902) == kIgpuOther && igpuGen(0x1586) == kIgpuOther,
              "registers are read only where the Linux headers give the block's base address: not on DCN 3.5 and 3.6, which take it from IP discovery");

extern "C" int rdmsr_carefully(uint32_t msr, uint32_t *lo, uint32_t *hi);

static constexpr uint32_t hz100(uint32_t f0, uint32_t f1, uint64_t ns) {
    return ns ? (uint32_t)((uint64_t)((f1 - f0) & 0xFFFFFFu) * 100000000000ULL / ns) : 0;
}
static_assert(hz100(0xFFFFF0u, 0x0006B0u, 12000000000ULL) == 14400, "1728 frames in 12 s across the wrap = 144.00 Hz");

void PrimeBridge::logf(const char *fmt, ...) {
    if (!logEnabled_) return;

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

    char stamped[900];
    int sn = snprintf(stamped, sizeof(stamped), "[%lld.%06lld] %s\n",
                      (long long)tv.tv_sec, (long long)tv.tv_usec, line);
    if (sn < 0) return;
    const size_t want = (size_t)sn < sizeof(stamped) ? (size_t)sn : sizeof(stamped) - 1;

    if (want > logBufCap_) return;

    if (logLock_) IOLockLock(logLock_);

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

static const char kLogPath[] = "/var/log/PrimeBridge.log";

void PrimeBridge::logFlush(bool sync) {
    if (!logEnabled_ || !logBuf_ || logBufLen_ == 0) return;

    vfs_context_t ctx = vfs_context_create(nullptr);
    if (!ctx) return;

    int fmode = FWRITE | O_APPEND | O_CREAT | O_NOFOLLOW;
    vnode_t vp = nullptr;
    if (!logReady_ && vnode_open(kLogPath, fmode, 0644, VNODE_LOOKUP_NOFOLLOW, &vp, ctx) == 0) {
        struct vnode_attr va;
        VATTR_INIT(&va);
        VATTR_WANTED(&va, va_data_size);
        if (vnode_getattr(vp, &va, ctx) == 0 && VATTR_IS_SUPPORTED(&va, va_data_size) && va.va_data_size > (8u << 20))
            fmode |= O_TRUNC;
        vnode_close(vp, FWRITE, ctx);
        logReady_ = true;
    }

    if (vnode_open(kLogPath, fmode, 0644, VNODE_LOOKUP_NOFOLLOW, &vp, ctx) == 0) {
        if (logLock_) IOLockLock(logLock_);
        int resid = 0;
        if (vn_rdwr(UIO_WRITE, vp, (caddr_t)logBuf_, (int)logBufLen_, 0, UIO_SYSSPACE, IO_APPEND | (sync ? IO_SYNC : 0),
                    vfs_context_ucred(ctx), &resid, kernproc) == 0)
            logBufLen_ = 0;
        if (logLock_) IOLockUnlock(logLock_);
        vnode_close(vp, FWRITE, ctx);
    }
    vfs_context_rele(ctx);
}

void PrimeBridge::logShutdown() {
    logFlush();
    if (logBuf_) { IOFree(logBuf_, logBufCap_); logBuf_ = nullptr; }
    logBufCap_ = 0;
    logBufLen_ = 0;
}

static bool gGaveUp = false;

IOService *PrimeBridge::probe(IOService *provider, SInt32 *score) {
    int dummy = 0;
    if (gGaveUp || PE_parse_boot_argn("-primeoff", &dummy, sizeof(dummy))) return nullptr;
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

    int dummy = 0;
    if (PE_parse_boot_argn("-primelogoff", &dummy, sizeof(dummy))) {
        logEnabled_ = false;
    }
    if (logEnabled_) {
        logBufCap_ = 256 * 1024;
        logBuf_ = static_cast<uint8_t *>(IOMalloc(logBufCap_));
        if (logBuf_) logBufLen_ = 0;
    }
    logf("PrimeBridge " PRIMEBRIDGE_VERSION " starting (log=%d)", logEnabled_ ? 1 : 0);

    if (!fb_.attach(igpu_)) { logf("firmware framebuffer not found or invalid"); return false; }

    {
        const uint32_t stridePx = fb_.stride() / 4u;
        logf("FB %ux%u bpp %u stride %u B stride_px %u padding_px %d phys 0x%llx flags 0x%llx BAR0 0x%llx +0x%llx off_in_bar0 0x%llx",
             fb_.width(), fb_.height(), fb_.depth(), fb_.stride(),
             stridePx, (int)stridePx - (int)fb_.width(),
             fb_.physAddr(), fb_.flagBits(), fb_.bar0Phys(), fb_.bar0Len(),
             fb_.physAddr() - fb_.bar0Phys());
    }

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
    symRedirect_ = OSSymbol::withCStringNoCopy("nvScanoutRedirect");

    PE_parse_boot_argn("primetest",   &testMode_,       sizeof(testMode_));
    PE_parse_boot_argn("primemirror", &mirrorExternal_, sizeof(mirrorExternal_));
    PE_parse_boot_argn("primedirect", &directVram_,     sizeof(directVram_));
    PE_parse_boot_argn("primefmt",    &pixelFmt_,       sizeof(pixelFmt_));
    if (pixelFmt_ > 4) pixelFmt_ = 4;
    setProperty("PixelFormat", pixelFmt_, 32);

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
        holdTicks_ = holdS * 4;
        logf("test pattern %u drawn (fmt %u), hold %u s during boot", testMode_, pixelFmt_, holdS);
    }
    setProperty("TestMode", testMode_, 32);

    uint32_t hz = 60;
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
        if (!sawNvfb_) {
            sawNvfb_ = true;
            logf("NVRMNVDAFramebuffer published, its scanout is not ready yet (rc 0x%x, %ux%u)", rc, req.width, req.height);
        }
    }
    iter->release();
    return found;
}

bool PrimeBridge::redirect(void *kva, uint64_t phys) {
    __asm__ volatile("sfence" ::: "memory");
    return symRedirect_ &&
           nvfb_->callPlatformFunction(symRedirect_, false, kva, reinterpret_cast<void *>(phys), nullptr, nullptr) == kIOReturnSuccess;
}

const void *PrimeBridge::ourKva() const {
    return directKva_ ? fb_.base() : staging_ ? staging_ : origNvKva_;
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

    nvAlloc_ = (req.mappedTotal >= nvSize_ && req.mappedTotal <= (128ULL << 20)) ? req.mappedTotal : 0;
    setProperty("NVFBAllocBytes", nvAlloc_, 64);
    if (!nvAlloc_)
        logf("NVRMFB reports a scanout allocation of %llu bytes (0: NVRMFB without the PrimeBridge patch, over 128 MiB: too large): "
             "no redirect, since a larger mode could overrun the staging buffer", (unsigned long long)req.mappedTotal);

    if (nvAlloc_ && directVram_ == 0) {
        const size_t frame = (size_t)nvPitch_ * nvHeight_;
        if (!staging_ || nvAlloc_ > stagingSize_) {
            if (staging_) { IOFreeAligned(staging_, stagingSize_); staging_ = nullptr; }
            if (shadow_)  { IOFreeAligned(shadow_, stagingSize_);  shadow_  = nullptr; }
            stagingSize_ = nvAlloc_;
            staging_ = static_cast<uint8_t *>(IOMallocAligned(stagingSize_, PAGE_SIZE));
            shadow_  = static_cast<uint8_t *>(IOMallocAligned(stagingSize_, PAGE_SIZE));
        }
        if (staging_) memcpy(staging_, origNvKva_, frame);
        if (staging_ && redirect(staging_, 0)) {
            if (shadow_) memset(shadow_, 0, frame);
            setProperty("PrimeMode", "staging-redirect-ce-8bit");
            logf("redirected NVRMNVDAFramebuffer scanout kva to cached staging (%llu bytes; NVRMFB allocation %llu, GOP mapping %llu)",
                  (unsigned long long)stagingSize_, (unsigned long long)nvAlloc_, (unsigned long long)fb_.mapLength());
        } else {
            if (staging_) logf("this NVRMFB has no nvScanoutRedirect (patched before PrimeBridge 0.6.0): no redirect, install this release's NVRMFB");
            if (staging_) IOFreeAligned(staging_, stagingSize_);
            if (shadow_)  IOFreeAligned(shadow_, stagingSize_);
            staging_ = shadow_ = nullptr;
            stagingSize_ = 0;
            setProperty("PrimeMode", "direct-vram-fallback");
        }
    } else {
        setProperty("PrimeMode", "direct-vram-fallback");
    }

    redirected_ = true;
    firstFrame_ = true;
    return true;
}

void PrimeBridge::probeIgpu() {
    const bool haveRef = shadow_ && pixelFmt_ == 1 && nvPitch_ == fb_.stride() && nvHeight_ >= fb_.height();
    const uint16_t dev = igpu_->configRead16(kIOPCIConfigDeviceID);
    const uint32_t gen = igpuGen(dev), fb = gen == kIgpuVega ? kRegFbOffsetVega : gen == kIgpuRembrandt ? kRegFbOffset : 0;
    IODeviceMemory *bar5 = gen ? igpu_->getDeviceMemoryWithRegister(kIOPCIConfigBaseAddress5) : nullptr;
    if (!mmio_ && bar5 && bar5->getLength() >= 0x80000)
        mmio_ = bar5->createMappingInTask(kernel_task, 0, kIOMapAnywhere | kIOMapInhibitCache, 0, 0x80000);
    if (!mmio_) {
        probed_ = true;
        logf("igpu probe: device 0x%04x, %s, BAR0 write benchmark only", dev, gen ? "BAR5 not mapped" : "no register map here");
        chooseWriteMapping(0);
        return;
    }
    regs_ = reinterpret_cast<volatile uint32_t *>(mmio_->getVirtualAddress());

    const uint32_t off = fb ? regs_[fb / 4] : 0xFFFFFFFFu, base = fb ? regs_[fb / 4 + kFbLocBase] : 0, top = fb ? regs_[fb / 4 + kFbLocTop] : 0;
    const uint64_t vramPhys = (uint64_t)(off & 0xFFFFFFu) << 24;
    const uint64_t vramSize = (top & 0xFFFFFFu) >= (base & 0xFFFFFFu)
                            ? (uint64_t)((top & 0xFFFFFFu) - (base & 0xFFFFFFu) + 1) << 24 : 0;
    const uint64_t gopOff = fb_.physAddr() - fb_.bar0Phys(), len = (uint64_t)fb_.stride() * fb_.height();
    if (fb) logf("igpu probe: FB_OFFSET 0x%08x FB_LOCATION 0x%08x..0x%08x -> VRAM at phys 0x%llx, %llu MB; GOP at VRAM +0x%llx",
                 off, base, top, vramPhys, vramSize >> 20, gopOff);

    IOMemoryMap *ram = nullptr;
    if (off != 0xFFFFFFFFu && vramPhys && gopOff + fb_.mapLength() <= vramSize) {
        if (IODeviceMemory *md = IODeviceMemory::withRange(vramPhys + gopOff, len)) {
            ram = md->createMappingInTask(kernel_task, 0, kIOMapAnywhere | kIOMapDefaultCache | kIOMapReadOnly);
            md->release();
        }
    }
    if (ram && haveRef) {
        volatile uint32_t *bar = reinterpret_cast<volatile uint32_t *>(fb_.base());
        const volatile uint32_t *mem = reinterpret_cast<const volatile uint32_t *>(ram->getVirtualAddress());
        uint32_t saved[32], same = 0;
        size_t at[32];
        for (uint32_t k = 0; k < 32; k++) {
            at[k] = (size_t)((2 * k + 1) * fb_.height() / 64) * (fb_.stride() / 4) + (2 * k + 1) * fb_.width() / 64;
            saved[k] = bar[at[k]];
            bar[at[k]] = 0x00A55A00u ^ (k * 0x00010101u);
        }
        __builtin_ia32_sfence();
        (void)bar[at[31]];
        for (uint32_t t = 0; t < 100 && same < 32; t++) {
            if (t) IODelay(100);
            for (uint32_t k = 0; k < 32; k++) __builtin_ia32_clflush((const void *)(uintptr_t)(mem + at[k]));
            __builtin_ia32_mfence();
            same = 0;
            for (uint32_t k = 0; k < 32; k++) same += mem[at[k]] == (0x00A55A00u ^ (k * 0x00010101u));
        }
        for (uint32_t k = 0; k < 32; k++) bar[at[k]] = saved[k];
        __builtin_ia32_sfence();
        ram->release();
        probed_ = true;
        logf("igpu probe: phys 0x%llx: %u of 32 pixels written through BAR0 read back there -> %s",
             vramPhys + gopOff, same, same == 32 ? "MATCH" : "NO MATCH");
        chooseWriteMapping(same == 32 ? vramPhys + gopOff : 0);
    } else {
        if (ram) ram->release();
        probed_ = true;
        if (fb) logf("igpu probe: no comparison (VRAM address implausible or not mappable, or no reference frame)");
        else logf("igpu probe: device 0x%04x, no VRAM register map here, BAR0 write benchmark only", dev);
        chooseWriteMapping(0);
    }

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

void PrimeBridge::chooseWriteMapping(uint64_t vramGop) {
    uint32_t carve = 1;
    PE_parse_boot_argn("primecarve", &carve, sizeof(carve));
    if (!carve || !shadow_ || pixelFmt_ != 1 || nvPitch_ != fb_.stride() || nvHeight_ < fb_.height()) return;
    uint32_t lo = 0, hi = 0;
    const bool wc = rdmsr_carefully(0x277, &lo, &hi) == 0 && ((hi >> 16) & 7u) == 1;
    logf("igpu writes: PAT 0x%08x%08x (PA6 %s)", hi, lo, wc ? "WC" : "not WC");
    auto bench = [this, wc]() -> uint64_t {
        uint64_t best = ~0ULL;
        for (uint32_t pass = 0; pass < (wc ? 4u : 1u) && best >= kFastRowNs; pass++) {
            const uint64_t t0 = mach_absolute_time();
            for (uint32_t k = 0; k < 64; k++) {
                const uint32_t y = (2 * k + 1) * fb_.height() / 128;
                fb_.blitRow(y, reinterpret_cast<const uint32_t *>(shadow_ + (uint64_t)y * nvPitch_), fb_.width(), pixelFmt_);
            }
            __builtin_ia32_sfence();
            uint64_t ns;
            absolutetime_to_nanoseconds(mach_absolute_time() - t0, &ns);
            if (ns / 64 < best) best = ns / 64;
        }
        return best;
    };
    fb_.useBootMapping();
    const uint64_t tBar = bench();
    const uint64_t tBarWC = fb_.mapWriteCombined(fb_.physAddr()) ? bench() : ~0ULL;
    const uint64_t tVram = vramGop && fb_.mapWriteCombined(vramGop) ? bench() : ~0ULL;
    const bool vram = tVram < tBarWC && tVram < tBar;
    uint64_t best = tVram;
    if (!vram) {
        if (tBarWC < tBar && fb_.mapWriteCombined(fb_.physAddr())) best = tBarWC;
        else { fb_.useBootMapping(); best = tBar; }
    }
    directPhys_ = vram ? vramGop : 0;
    vramGop_ = vramGop;
    fastWrites_ = best < kFastRowNs;
    rebench_ = !fastWrites_ && wc && ++benchTries_ <= 8;
    setProperty("PrimeMode", vram ? "staging-redirect-carveout" : "staging-redirect-ce-8bit");
    logf("igpu writes: ns per row: BAR0 %llu, BAR0 write-combined %lld, VRAM write-combined %lld -> %s%s%s",
         tBar, (long long)tBarWC, (long long)tVram, vram ? "VRAM at phys" : "BAR0", fastWrites_ ? ", fast" : "",
         rebench_ ? ", slow with PA6 WC: measuring again later" : "");
}

void PrimeBridge::tickNullMoth(IOTimerEventSource *t) {
    if (colorTest_ == 1) {
        if (++colorStageTicks_ >= 8) {
            colorStageTicks_ = 0;
            colorStage_ = (colorStage_ + 1) % 6;
            fb_.drawCalibration(colorStage_);
            logf("calibration mire: stage %u", colorStage_);
        }
        if ((++logTick_ & 15u) == 0) logFlush(false);
        t->setTimeoutMS(250);
        return;
    }

    if ((++logTick_ & 15u) == 0) logFlush(false);

    if (!nvfb_) {
        findNvidiaFramebuffer();
        if (!nvfb_) {
            if (testMode_ && holdTicks_ > 0) {
                holdTicks_--;
                fb_.drawTestPattern(testMode_ == 3 ? 2 : testMode_, pixelFmt_);
            }
            if (++waitTicks_ == 240) {
                char why[16] = "";
                bool nvrm = false;
                if (OSDictionary *m = serviceMatching("NVRM")) {
                    if (IOService *s = copyMatchingService(m)) {
                        nvrm = true;
                        if (OSString *w = OSDynamicCast(OSString, s->getProperty("nvfbforce"))) strlcpy(why, w->getCStringNoCopy(), sizeof why);
                        s->release();
                    }
                    m->release();
                }
                logf("no NVIDIA head after 60 s: %s", !nvrm ? "NullMoth's NVRM is not running"
                     : !strcmp(why, "no-hdmi") ? "this NVIDIA GPU has no HDMI port to force, so it cannot drive the panel"
                     : !strcmp(why, "refused") ? "NVKMS refused the forced HDMI display"
                     : !strcmp(why, "off") ? "boot-arg nvfbforce=1 is missing"
                     : !*why ? "NVRMFB is not the one install.sh built for this NullMoth version: run install.sh again"
                     : "NVRMFB has a display but started no framebuffer (see the NVRM-fb lines of the kernel log)");
                setProperty("PrimeMode", "no-nvidia-head");
                logFlush();
            }
            if (waitTicks_ == 300) {
                logf("still no NVIDIA head after 2 min: PrimeBridge steps aside so macOS's basic display driver can use the panel");
                logFlush();
                gGaveUp = true;
                terminate();
                return;
            }
            t->setTimeoutMS(waitTicks_ < 240 ? 250 : 1000);
            return;
        }
        OSObject *forced = nvfb_->copyProperty("nvfbforce-display", gIOServicePlane,
                                               kIORegistryIterateRecursively | kIORegistryIterateParents);
        const OSNumber *num = OSDynamicCast(OSNumber, forced);
        const bool realDisplay = num && num->unsigned64BitValue() == 0;
        if (forced) forced->release();
        waitTicks_ = 0;
        if (!num)
            logf("this NVRMFB does not publish nvfbforce-display (patched before PrimeBridge 0.5.1): "
                 "a monitor plugged into the NVIDIA GPU would freeze once redirected");
        if (realDisplay && !mirrorExternal_) {
            mirrorExternal_ = 1;
            logf("a display is plugged into the NVIDIA GPU (nvfbforce forced nothing): it keeps working, "
                 "and the panel mirrors its top-left %ux%u", fb_.width(), fb_.height());
        }
    }

    if (!redirected_) {
        int iopArmed = 0;
        size_t iopSz = sizeof(iopArmed);
        sysctlbyname("debug.nvaccel_iop", &iopArmed, &iopSz, nullptr, 0);

        if (iopArmed != 1 || (testMode_ == 3 && holdTicks_ > 0)) {
            if (testMode_ && holdTicks_ > 0) {
                holdTicks_--;
                fb_.drawTestPattern(testMode_ == 3 ? 2 : testMode_, pixelFmt_);
            }
            if (iopArmed != 1 && ++waitTicks_ == 1)
                logf("waiting for NVAccel's present path (debug.nvaccel_iop %d)", iopArmed);
            if (waitTicks_ == 300) {
                logf("NVIDIA head found, but NVAccel has not armed its present path after 60 s (debug.nvaccel_iop %d): check nvaccel=1",
                     iopArmed);
                setProperty("PrimeMode", "no-nvaccel-present");
                logFlush();
            }
            t->setTimeoutMS(200);
            return;
        }

        if (waitTicks_) {
            logf("NVAccel's present path armed after about %u ms", waitTicks_ * 200);
            waitTicks_ = 0;
        }

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

    if ((directKva_ || (statTick_ & 63u) == 0) && redirected_) {
        NVRMVramRequest req {};
        req.version = 2;
        if (nvfb_->callPlatformFunction(symScanoutInfo_, false, &req, nullptr, nullptr, nullptr) == kIOReturnSuccess) {
            const void *ours = ourKva();
            if (req.kva && (req.width != nvWidth_ || req.height != nvHeight_ || req.pitch != nvPitch_ ||
                            req.kva != ours)) {
                logf("scanout changed: %ux%u p%u -> %ux%u p%u; NVRMFB gets its own buffer back, re-attaching",
                     nvWidth_, nvHeight_, nvPitch_, req.width, req.height, req.pitch);
                if (ours != origNvKva_) redirect(nullptr, 0);
                redirected_ = false;
                directKva_ = false;
                t->setTimeoutMS(200);
                return;
            }
        }
    }

    const uint32_t w = nvWidth_  < fb_.width()  ? nvWidth_  : fb_.width();
    const uint32_t h = nvHeight_ < fb_.height() ? nvHeight_ : fb_.height();
    const size_t   rowBytes = (size_t)w * 4;

    bool anyRowChanged = false;

    const uint64_t workStart = mach_absolute_time();
    if (!directKva_) {
        int64_t iopOk = -1;
        size_t iopOkSz = sizeof(iopOk);
        const bool newFrame = sysctlbyname("debug.nvaccel_iop_ok", &iopOk, &iopOkSz, nullptr, 0) != 0 || iopOk != iopOk_;
        iopOk_ = iopOk;
        uint8_t *mirrorDst = (staging_ && mirrorExternal_) ? origNvKva_ : nullptr;
        const uint32_t rows = mirrorDst ? nvHeight_ : h;
        const size_t cmpBytes = mirrorDst ? (size_t)nvWidth_ * 4 : rowBytes;
        for (uint32_t y = 0; (firstFrame_ || newFrame) && y < rows; y++) {
            const uint32_t *srcRow = reinterpret_cast<const uint32_t *>(srcBuf + (uint64_t)y * nvPitch_);
            uint32_t       *shdRow = shadow_ ? reinterpret_cast<uint32_t *>(shadow_ + (uint64_t)y * nvPitch_) : nullptr;

            if (firstFrame_ || !shdRow || memcmp(srcRow, shdRow, cmpBytes) != 0) {
                if (shdRow) memcpy(shdRow, srcRow, cmpBytes);
                if (mirrorDst) memcpy(mirrorDst + (uint64_t)y * nvPitch_, srcRow, cmpBytes);
                if (y < h) {
                    const uint64_t b = mach_absolute_time();
                    fb_.blitRow(y, srcRow, w, pixelFmt_);
                    statBlit_ += mach_absolute_time() - b;
                    statRows_++;
                }
                anyRowChanged = true;
            }
        }
        if (anyRowChanged) {
            if (!framesCopied_) logf("first frame on the panel");
            firstFrame_ = false;
            framesCopied_++;
        }
    }

    if (anyRowChanged) __builtin_ia32_sfence();
    const uint64_t work = mach_absolute_time() - workStart;
    statWork_ += work;
    if (work > statWorkMax_) statWorkMax_ = work;

    if (anyRowChanged) {
        idleTicks_ = 0;
        if (lowFreqMode_) { lowFreqMode_ = false; }
    } else {
        idleTicks_++;
    }
    if (!lowFreqMode_ && idleTicks_ >= 30) lowFreqMode_ = true;

    if (++statTick_ >= 720) {
        statTick_ = 0;
        setProperty("FramesCopied", framesCopied_, 64);
        setProperty("EffectiveRateHz", (uint64_t)(1000000 / tickUs()), 64);
        uint64_t workNs, maxNs, blitNs;
        absolutetime_to_nanoseconds(statWork_, &workNs);
        absolutetime_to_nanoseconds(statWorkMax_, &maxNs);
        absolutetime_to_nanoseconds(statBlit_, &blitNs);
        uint32_t panelHz100 = 0;
        if (otgFrameReg_) {
            const uint32_t f = regs_[otgFrameReg_ / 4];
            const uint64_t at = mach_absolute_time();
            uint64_t ns;
            absolutetime_to_nanoseconds(at - otgAbs_, &ns);
            panelHz100 = hz100(otgFrames_, f, ns);
            if (panelHz100) setProperty("PanelHz", (uint64_t)((panelHz100 + 50) / 100), 32);
            otgFrames_ = f;
            otgAbs_ = at;
        }
        logf("rate-hint: %u Hz%s, work avg %llu us max %llu us, GOP writes avg %llu us, rows/tick %llu, panel %u.%02u Hz%s",
             1000000 / tickUs(), lowFreqMode_ ? " (idle)" : "",
             workNs / 720000, maxNs / 1000, blitNs / 720000, statRows_ / 720, panelHz100 / 100, panelHz100 % 100,
             directKva_ ? ", direct" : "");
        statWork_ = statWorkMax_ = statBlit_ = statRows_ = 0;
        logFlush(false);
    }
    if (!probed_ && !anyRowChanged && framesCopied_) probeIgpu();
    else if (rebench_ && !directKva_ && statTick_ == 0) chooseWriteMapping(vramGop_);
    if (fastWrites_ && !directKva_ && redirected_ && staging_ && pixelFmt_ == 1 && !mirrorExternal_ &&
        nvAlloc_ && nvAlloc_ <= fb_.mapLength() &&
        nvWidth_ == fb_.width() && nvHeight_ == fb_.height() && nvPitch_ == fb_.stride() &&
        redirect(fb_.base(), directPhys_)) {
        directKva_ = true;
        statTick_ = 712;
        setProperty("PrimeMode", directPhys_ ? "direct-copy-engine" : "direct-kva");
        setProperty("EffectiveRateHz", (uint64_t)(1000000 / tickUs()), 64);
        logf("NVAccel now puts its frames straight into the panel's buffer%s (no staging copy, no diff)",
             directPhys_ ? ", with the GPU copy engine" : "");
        logFlush();
    }

    uint64_t period, now = mach_absolute_time();
    clock_interval_to_absolutetime_interval(tickUs(), kMicrosecondScale, &period);
    nextTickAbs_ = nextTickAbs_ + period > now ? nextTickAbs_ + period : now + period;
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

    if (nvfb_ && ourKva() != origNvKva_) redirect(nullptr, 0);
    const bool keepMaps = staging_ != nullptr;
    if (keepMaps)
        logf("staging buffer (%llu bytes) and panel mapping left allocated: NVAccel may still be copying a frame into them",
             (unsigned long long)stagingSize_);
    if (shadow_) IOFreeAligned(shadow_, stagingSize_);
    shadow_ = staging_ = nullptr;
    stagingSize_ = 0;

    if (nvfb_) { nvfb_->release(); nvfb_ = nullptr; }
    if (symScanoutInfo_) { symScanoutInfo_->release(); symScanoutInfo_ = nullptr; }
    if (symRedirect_) { symRedirect_->release(); symRedirect_ = nullptr; }

    if (lock_) {
        IOLockLock(lock_);
        if (source_) { source_->release(); source_ = nullptr; }
        IOLockUnlock(lock_);
        IOLockFree(lock_); lock_ = nullptr;
    }
    logShutdown();
    if (logLock_) { IOLockFree(logLock_); logLock_ = nullptr; }
    if (mmio_) { mmio_->release(); mmio_ = nullptr; regs_ = nullptr; otgFrameReg_ = 0; }
    if (!keepMaps) fb_.detach();
    if (igpu_) { igpu_->release(); igpu_ = nullptr; }
}

void PrimeBridge::stop(IOService *provider) { cleanup(); IOService::stop(provider); }

void PrimeBridge::detach(IOService *provider) {
    IOService::detach(provider);
    if (gGaveUp) provider->registerService();
}

uint32_t PrimeBridge::tickUs() const {
    return directKva_ ? 250000u : !staging_ ? 100000u : lowFreqMode_ ? 50000u : periodUs_;
}
