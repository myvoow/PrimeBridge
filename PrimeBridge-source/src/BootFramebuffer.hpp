#pragma once
#include <IOKit/pci/IOPCIDevice.h>
#include "FrameSource.hpp"

// The framebuffer programmed by the firmware (GOP) on the iGPU, which the DCN
// keeps scanning out. On AMD Rembrandt (Radeon 680M 0x1681) the GOP is standard
// BGRX8888 (byte0=B, byte1=G, byte2=R, byte3=X) — the same order macOS writes.
// The packPixel default (fmt=1) matches this; no crossbar correction is needed.
class BootFramebuffer {
public:
    static constexpr uint32_t kMaxCursorDim = 64;

    bool attach(IOPCIDevice *igpu);
    void detach();
    // Remap the pixels write-combined at phys: BAR0's GOP range, or the 680M's VRAM in system RAM.
    bool mapWriteCombined(uint64_t phys);
    // mode 1: 8 colour bars (W Y C G M R B K)
    // mode 2: diagnostic (R/G/B/W bars, 64 px grid, border, grey ramp)
    void drawTestPattern(uint32_t mode = 1, uint32_t fmt = 0);
    void blitFromCPU(const PrimeFrame &f, uint32_t fmt);

    // Convert and write a single scanline (without cursor) to the GOP framebuffer
    void blitRow(uint32_t y, const uint32_t *srcRow, uint32_t w, uint32_t fmt);

    // Calibration mire (primecolors=1). Writes a RAW, no-packing probe pattern so
    // the DCN crossbar can be measured independently of packPixel. stage 0..3 =
    // exactly one byte set to 0xFF (byte0, byte1, byte2, byte3); stage 4 = all
    // white; stage 5 = 1-px checkerboard (reveals a wrong stride as vertical
    // stripes). Logs the bytes actually written.
    void drawCalibration(uint32_t stage);

    uint8_t *base()     const { return base_; }
    uint32_t width()    const { return width_; }
    uint32_t height()   const { return height_; }
    uint32_t stride()   const { return stride_; }
    uint32_t depth()    const { return depth_; }
    uint64_t physAddr() const { return phys_; }
    uint64_t bar0Phys() const { return bar0_; }
    uint64_t bar0Len()  const { return bar0Len_; }
    uint64_t flagBits() const { return flags_; }
private:
    void drawDiagnostic(uint32_t fmt);
    IOMemoryMap *map_  {nullptr};
    uint8_t     *base_ {nullptr};
    uint64_t     phys_ {0}, bar0_ {0}, bar0Len_ {0}, flags_ {0};
    uint32_t     width_ {0}, height_ {0}, stride_ {0}, depth_ {0};
};
