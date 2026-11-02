#include "BootFramebuffer.hpp"
#include <IOKit/IOPlatformExpert.h>
#include <IOKit/IODeviceMemory.h>
#include <pexpert/pexpert.h>
#include <string.h>

// Pixel packing. The source is macOS 32BGRA (byte0=B byte1=G byte2=R byte3=A=0xFF).
// The AMD Rembrandt GOP is standard BGRX8888 (byte0=B byte1=G byte2=R byte3=X),
// the same order macOS writes. fmt=1 (default) zeroes the reserved byte.
// fmt=4 was previously "calibrated" against a one-byte-shifted base address and
// is kept only as a comparison option, not a correct packing.
static inline uint32_t packPixel(uint32_t p, uint32_t fmt) {
    const uint32_t b = p & 0xFFu;
    const uint32_t g = (p >> 8) & 0xFFu;
    const uint32_t r = (p >> 16) & 0xFFu;
    switch (fmt) {
    case 1:
        // 8-bit BGRX8888 (byte3 forced 0). Normal GOP order, matches macOS BGRA.
        return p & 0x00FFFFFFu;
    case 2:
        // 8-bit RGBX8888 (R/B swapped, byte3 forced 0).
        return (b << 16) | (g << 8) | r;
    case 4:
        // Comparison-only: G/B swapped. Was "calibrated" against a base address
        // shifted by one byte (flag bits), so its "crossbar" was an artefact.
        return (r << 16) | (b << 8) | g;
    case 0:
        // Legacy "Mirrored Blue" (v0.3.1): byte0=B, byte1=G, byte2=R, byte3=B.
        return (p & 0x00FFFFFFu) | (b << 24);
    default: // case 3 and anything else
        // Raw 32BGRA passthrough (v0.2.0 behavior).
        return p;
    }
}

bool BootFramebuffer::attach(IOPCIDevice *igpu) {
    PE_Video v {};
    if (IOService::getPlatform()->getConsoleInfo(&v) != kIOReturnSuccess) {
        IOLog("PrimeBridge: getConsoleInfo failed\n");
        return false;
    }
    // Log the raw values first, so a refusal below can still be diagnosed.
    v.v_pixelFormat[sizeof(v.v_pixelFormat) - 1] = 0;
    IOLog("PrimeBridge: console base 0x%llx %lux%lu depth %lu rowBytes %lu display %lu "
          "fmt '%s' offset %lu length %lu rotate %u scale %u\n",
          (unsigned long long)v.v_baseAddr, v.v_width, v.v_height, v.v_depth, v.v_rowBytes,
          v.v_display, v.v_pixelFormat, v.v_offset, v.v_length,
          (unsigned)v.v_rotate, (unsigned)v.v_scale);
    depth_ = (uint32_t)v.v_depth;
    // Refuse anything but 32 bpp: all blit paths write 8 bits per channel.
    if (v.v_depth != 32) {
        IOLog("PrimeBridge: refusing GOP depth %lu (only 32 bpp supported)\n", v.v_depth);
        return false;
    }
    if (!v.v_baseAddr || !v.v_width || !v.v_height) return false;

    // CRITICAL: v_baseAddr bit 0 is a flag (XNU: "unless b0 is set to indicate
    // physical"). Mask it and the rest of the low bits off like XNU does:
    //   new_vinfo.v_physaddr = boot_vinfo->v_baseAddr & ~3UL;
    // A base shifted by one byte recolours every pixel (R'=G, G'=B, B'=garbage),
    // which manifestly cannot be fixed by any channel packing. Mask with ~3ULL,
    // NOT ~0xFFF (the page-alignment mask from RDNA4FB was wrong for this flag).
    const uint64_t rawBase = (uint64_t)v.v_baseAddr;
    const uint64_t flags   = rawBase & 3ULL;
    phys_ = rawBase & ~3ULL;
    flags_ = flags;
    IOLog("PrimeBridge: v_baseAddr raw 0x%llx -> masked 0x%llx (flag bits 0x%llx)%s\n",
          rawBase, phys_, flags, flags ? "  *** FLAG BITS PRESENT ***" : "");
    if (phys_ & 0xFFFULL) {
        // Should not happen: the scanout base is expected page-aligned. Log, but
        // keep the value rather than masking further (masking more would re-shift
        // the aperture like the original bug).
        IOLog("PrimeBridge: WARNING masked base 0x%llx is not 4K-aligned\n", phys_);
    }

    width_  = (uint32_t)v.v_width;
    height_ = (uint32_t)v.v_height;
    stride_ = (uint32_t)v.v_rowBytes;
    if (stride_ < width_ * 4) {
        IOLog("PrimeBridge: rowBytes %u < width*4 %u, refusing\n", stride_, width_ * 4);
        return false;
    }
    const uint64_t len = (uint64_t)stride_ * height_;

    // Guard: on an APU the GOP framebuffer must lie inside the iGPU's BAR0.
    // Otherwise refuse, rather than write to arbitrary memory.
    IODeviceMemory *bar0 = igpu->getDeviceMemoryWithRegister(kIOPCIConfigBaseAddress0);
    if (!bar0) { IOLog("PrimeBridge: iGPU has no BAR0\n"); return false; }
    bar0_ = bar0->getPhysicalAddress(); bar0Len_ = bar0->getLength();
    if (phys_ < bar0_ || phys_ + len > bar0_ + bar0Len_) {
        IOLog("PrimeBridge: GOP 0x%llx outside BAR0 [0x%llx +0x%llx]\n", phys_, bar0_, bar0Len_);
        return false;
    }
    map_ = bar0->createMappingInTask(kernel_task, 0,
              kIOMapAnywhere | kIOMapWriteCombineCache, phys_ - bar0_, len);
    if (!map_) { IOLog("PrimeBridge: mapping failed\n"); return false; }
    base_ = reinterpret_cast<uint8_t *>(map_->getVirtualAddress());
    return true;
}

bool BootFramebuffer::mapWriteCombined(uint64_t phys) {
    // XNU fixes a physical range's cache mode when its descriptor is created and defaults unmanaged memory
    // to uncached; kIOMapWriteCombineCache at map time alone left both BAR0 and the VRAM at 120-260 MB/s.
    const IOOptionBits kCacheShift = 28;   // kIOMemoryBufferCacheShift, XNU_KERNEL_PRIVATE in IOMemoryDescriptor.h
    IOMemoryDescriptor *md = IOMemoryDescriptor::withAddressRange(phys, (uint64_t)stride_ * height_,
        kIODirectionInOut | kIOMemoryMapperNone | (kIOWriteCombineCache << kCacheShift), TASK_NULL);
    IOMemoryMap *m = md ? md->createMappingInTask(kernel_task, 0, kIOMapAnywhere | kIOMapWriteCombineCache) : nullptr;
    if (md) md->release();
    if (!m) return false;
    if (map_) map_->release();
    map_ = m;
    base_ = reinterpret_cast<uint8_t *>(m->getVirtualAddress());
    return true;
}

void BootFramebuffer::detach() {
    if (map_) { map_->release(); map_ = nullptr; base_ = nullptr; }
}

void BootFramebuffer::drawTestPattern(uint32_t mode, uint32_t fmt) {
    if (!base_) return;
    if (mode == 2) { drawDiagnostic(fmt); return; }
    // Expected left to right: white, yellow, cyan, green, magenta, red, blue, black.
    static const uint32_t bars[8] = { 0xFFFFFF, 0xFFFF00, 0x00FFFF, 0x00FF00,
                                      0xFF00FF, 0xFF0000, 0x0000FF, 0x000000 };
    for (uint32_t y = 0; y < height_; y++) {
        uint32_t *row = reinterpret_cast<uint32_t *>(base_ + (uint64_t)y * stride_);
        for (uint32_t x = 0; x < width_; x++) {
            row[x] = packPixel(bars[(x * 8) / width_], fmt);
        }
    }
    __builtin_ia32_sfence();
}

void BootFramebuffer::drawDiagnostic(uint32_t fmt) {
    static const uint32_t top[4] = { 0xFF0000, 0x00FF00, 0x0000FF, 0xFFFFFF };
    const uint32_t h3 = height_ / 3;
    for (uint32_t y = 0; y < height_; y++) {
        uint32_t *row = reinterpret_cast<uint32_t *>(base_ + (uint64_t)y * stride_);
        for (uint32_t x = 0; x < width_; x++) {
            uint32_t c;
            if (y < 2 || y >= height_ - 2 || x < 2 || x >= width_ - 2) c = 0xFFFFFF;
            else if (y < h3)       c = top[(x * 4) / width_];
            else if (y < 2 * h3)   c = ((x & 63) == 0 || (y & 63) == 0) ? 0xFFFFFF : 0x000000;
            else { uint32_t g = (x * 255) / (width_ - 1); c = (g << 16) | (g << 8) | g; }
            row[x] = packPixel(c, fmt);
        }
    }
    __builtin_ia32_sfence();
}

void BootFramebuffer::drawCalibration(uint32_t stage) {
    if (!base_) return;
    const char *names[6] = { "byte0", "byte1", "byte2", "byte3", "white", "checker1px" };
    const uint32_t pattern[5] = { 0x000000FFu, 0x0000FF00u, 0x00FF0000u, 0xFF000000u, 0xFFFFFFFFu };

    for (uint32_t y = 0; y < height_; y++) {
        uint8_t *row = base_ + (uint64_t)y * stride_;
        for (uint32_t x = 0; x < width_; x++) {
            if (stage < 5) {
                const uint32_t p = pattern[stage];
                row[x * 4 + 0] = p & 0xFF;
                row[x * 4 + 1] = (p >> 8) & 0xFF;
                row[x * 4 + 2] = (p >> 16) & 0xFF;
                row[x * 4 + 3] = (p >> 24) & 0xFF;
            } else {
                // stage 5: 1-pixel checkerboard. A wrong stride desynchronises the
                // next row and shows up as vertical stripes / diagonal ghosting.
                const uint8_t on = ((x ^ y) & 1) ? 0xFF : 0x00;
                row[x * 4 + 0] = on;
                row[x * 4 + 1] = on;
                row[x * 4 + 2] = on;
                row[x * 4 + 3] = on;
            }
        }
    }
    __builtin_ia32_sfence();
    IOLog("PrimeBridge: calibration stage %u (%s) written, bytes = 0x%08X\n",
          stage, stage < 6 ? names[stage] : "?", stage < 5 ? pattern[stage] : 0xFFFFFFFFu);
}

void BootFramebuffer::blitFromCPU(const PrimeFrame &f, uint32_t fmt) {
    if (!base_ || !f.cpuMap || f.format != kPrimeFormatXRGB8888) return;
    const uint8_t *src = reinterpret_cast<const uint8_t *>(f.cpuMap->getVirtualAddress());
    const uint32_t w = f.width  < width_  ? f.width  : width_;
    const uint32_t h = f.height < height_ ? f.height : height_;
    for (uint32_t y = 0; y < h; y++) {
        const uint32_t *srcRow = reinterpret_cast<const uint32_t *>(src + (uint64_t)y * f.stride);
        blitRow(y, srcRow, w, fmt);
    }
    __builtin_ia32_sfence();
}

void BootFramebuffer::blitRow(uint32_t y, const uint32_t *srcRow, uint32_t w, uint32_t fmt) {
    if (!base_ || !srcRow || y >= height_) return;
    if (w > width_) w = width_;
    uint32_t *dstRow = reinterpret_cast<uint32_t *>(base_ + (uint64_t)y * stride_);

    // Fast path: pack 2 pixels per 64-bit store into Write-Combining BAR0 memory.
    uint64_t *dst64 = reinterpret_cast<uint64_t *>(dstRow);
    const uint32_t pairs = w >> 1;
    if (fmt == 3) {
        // Raw passthrough: no per-pixel transform.
        for (uint32_t i = 0; i < pairs; i++) {
            dst64[i] = (uint64_t)srcRow[i * 2] | ((uint64_t)srcRow[i * 2 + 1] << 32);
        }
        if (w & 1) dstRow[w - 1] = srcRow[w - 1];
        return;
    }

    for (uint32_t i = 0; i < pairs; i++) {
        const uint32_t out0 = packPixel(srcRow[i * 2], fmt);
        const uint32_t out1 = packPixel(srcRow[i * 2 + 1], fmt);
        dst64[i] = (uint64_t)out0 | ((uint64_t)out1 << 32);
    }
    if (w & 1) {
        dstRow[w - 1] = packPixel(srcRow[w - 1], fmt);
    }
}
