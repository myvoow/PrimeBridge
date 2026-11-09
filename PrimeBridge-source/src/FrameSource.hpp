#pragma once
#include <libkern/c++/OSObject.h>
#include <IOKit/IOMemoryDescriptor.h>

enum PrimePixelFormat : uint32_t { kPrimeFormatXRGB8888 = 0 };

struct PrimeFrame {
    bool             valid;
    IOMemoryMap     *cpuMap;
    uint64_t         gpuVA;
    uint32_t         width, height, stride;
    PrimePixelFormat format;
    uint64_t         seq;
    void            *cookie;
};

class PrimeFrameSource : public OSObject {
    OSDeclareAbstractStructors(PrimeFrameSource)
public:
    virtual bool acquireFrame(PrimeFrame *out) = 0;
    virtual void releaseFrame(PrimeFrame *frame) = 0;
    virtual IOReturn gpuCopyTo(const PrimeFrame &, uint64_t,
                               uint32_t) { return kIOReturnUnsupported; }
};
