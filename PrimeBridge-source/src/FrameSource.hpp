#pragma once
#include <libkern/c++/OSObject.h>
#include <IOKit/IOMemoryDescriptor.h>

// v1: a single format, the firmware framebuffer's (32 bpp, BGRX in memory)
enum PrimePixelFormat : uint32_t { kPrimeFormatXRGB8888 = 0 };

struct PrimeFrame {
    bool             valid;
    IOMemoryMap     *cpuMap;    // v0: CPU-readable surface (through the RTX BAR)
    uint64_t         gpuVA;     // v1: GPU address of the surface, for the copy engine
    uint32_t         width, height, stride;
    PrimePixelFormat format;
    uint64_t         seq;       // incremented on every flip
    void            *cookie;    // reserved for the provider
};

// Implemented on the NVIDIA side (patched NVRMFB). PrimeBridge only knows this.
class PrimeFrameSource : public OSObject {
    OSDeclareAbstractStructors(PrimeFrameSource)
public:
    virtual bool acquireFrame(PrimeFrame *out) = 0;   // last displayed surface
    virtual void releaseFrame(PrimeFrame *frame) = 0;
    // v1: VRAM -> system RAM copy by the RTX copy engine
    virtual IOReturn gpuCopyTo(const PrimeFrame &, uint64_t /*dstBusAddr*/,
                               uint32_t /*dstStride*/) { return kIOReturnUnsupported; }
};
