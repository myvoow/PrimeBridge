#pragma once
#include <IOKit/pci/IOPCIDevice.h>
#include "FrameSource.hpp"

class BootFramebuffer {
public:
    static constexpr uint32_t kMaxCursorDim = 64;

    bool attach(IOPCIDevice *igpu);
    void detach();
    bool mapWriteCombined(uint64_t phys);
    void useBootMapping();
    void drawTestPattern(uint32_t mode = 1, uint32_t fmt = 0);
    void blitFromCPU(const PrimeFrame &f, uint32_t fmt);

    void blitRow(uint32_t y, const uint32_t *srcRow, uint32_t w, uint32_t fmt);

    void drawCalibration(uint32_t stage);

    uint8_t *base()     const { return base_; }
    uint32_t width()    const { return width_; }
    uint32_t height()   const { return height_; }
    uint32_t stride()   const { return stride_; }
    uint32_t depth()    const { return depth_; }
    uint64_t physAddr() const { return phys_; }
    uint64_t mapLength() const { return mapLen_; }
    uint64_t bar0Phys() const { return bar0_; }
    uint64_t bar0Len()  const { return bar0Len_; }
    uint64_t flagBits() const { return flags_; }
private:
    void drawDiagnostic(uint32_t fmt);
    IOMemoryMap *map_  {nullptr};
    IOMemoryMap *boot_ {nullptr};
    uint8_t     *base_ {nullptr};
    uint64_t     phys_ {0}, bar0_ {0}, bar0Len_ {0}, flags_ {0}, mapLen_ {0};
    uint32_t     width_ {0}, height_ {0}, stride_ {0}, depth_ {0};
};
