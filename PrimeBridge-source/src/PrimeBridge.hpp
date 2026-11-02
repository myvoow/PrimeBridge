#pragma once
#include <IOKit/IOService.h>
#include <IOKit/IOWorkLoop.h>
#include <IOKit/IOTimerEventSource.h>
#include <IOKit/IOLocks.h>
#include <libkern/c++/OSSymbol.h>
#include <sys/param.h>
#include "BootFramebuffer.hpp"

// ABI v2 request struct expected by NVRMNVDAFramebuffer::callPlatformFunction("nvScanoutInfo", ...)
// Mirrors nvrm_vram_abi.h exactly (NVRM_VRAM_ABI_VERSION == 2). sizeof == 0x50.
struct NVRMVramRequest {
    uint32_t version;       // +0x00 : must == 2
    uint32_t flags;         // +0x04
    uint64_t size;          // +0x08
    uint64_t actualSize;    // +0x10 : out = height * pitch
    uint64_t phys;          // +0x18 : out = BAR1 physical address
    void    *kva;           // +0x20 : out = kernel virtual address (fKva)
    void    *handle;        // +0x28 : out = NvKmsKapiMemory *
    uint64_t mappedTotal;   // +0x30
    uint32_t width;         // +0x38 : out = width
    uint32_t height;        // +0x3c : out = height
    uint32_t pitch;         // +0x40 : out = pitch
    uint32_t _pad;          // +0x44 (alignment to 8-byte)
    uint64_t allocationCookie; // +0x48
};
static_assert(sizeof(NVRMVramRequest) == 0x50, "NVRMVramRequest must be 0x50 bytes (matches nvrm_vram_abi.h)");

class PrimeBridge : public IOService {
    OSDeclareDefaultStructors(PrimeBridge)
public:
    IOService *probe(IOService *provider, SInt32 *score) override;
    bool start(IOService *provider) override;
    void stop(IOService *provider) override;

    // Optional explicit source registration API
    bool registerFrameSource(PrimeFrameSource *src);
    void unregisterFrameSource(PrimeFrameSource *src);
private:
    bool setup(IOService *provider);
    void cleanup();
    void copyTick(IOTimerEventSource *sender);
    void tickNullMoth(IOTimerEventSource *sender);
    bool findNvidiaFramebuffer();
    bool attachNvidiaScanout();
    void probeIgpu();
    void chooseWriteMapping(uint64_t vramGop);
    void logf(const char *fmt, ...) __printflike(2, 3);
    void logFlush();
    void logShutdown();

    IOPCIDevice        *igpu_            {nullptr};
    BootFramebuffer     fb_;
    IOWorkLoop         *wl_              {nullptr};
    IOTimerEventSource *timer_           {nullptr};
    IOLock             *lock_            {nullptr};
    IOLock             *logLock_         {nullptr};
    PrimeFrameSource   *source_          {nullptr};

    // NullMoth NVRMNVDAFramebuffer + NVAccel integration
    const OSSymbol     *symScanoutInfo_  {nullptr};
    IOService          *nvfb_            {nullptr};
    uint8_t            *origNvKva_       {nullptr};
    uint8_t            *staging_         {nullptr};
    uint8_t            *shadow_          {nullptr};
    size_t              stagingSize_     {0};
    uint64_t            nvPhys_          {0};
    uint64_t            nvSize_          {0};
    uint32_t            nvWidth_         {0};
    uint32_t            nvHeight_        {0};
    uint32_t            nvPitch_         {0};
    bool                redirected_      {false};
    bool                offsetsVerified_ {false};
    bool                firstFrame_      {true};
    uint32_t            mirrorExternal_  {0};  // 0 = off (fastest, internal-only); primemirror=1 re-enables external mirror
    uint32_t            directVram_      {0};
    uint32_t            pixelFmt_        {1};  // default: BGRX8888 (byte3=0), normal GOP/macOS order

    // Stats & diagnostics
    uint64_t            framesCopied_    {0};
    uint32_t            flipCleared_     {0};
    uint32_t            statTick_        {0};
    uint64_t            statWork_        {0};   // absolute-time sums over the 720-tick log window
    uint64_t            statWorkMax_     {0};
    uint64_t            statBlit_        {0};
    uint64_t            statRows_        {0};

    // Read-only 680M diagnostics: its register BAR and the running OTG's frame counter (panel refresh)
    IOMemoryMap        *mmio_            {nullptr};
    volatile uint32_t  *regs_            {nullptr};
    uint32_t            otgFrameReg_     {0};   // byte offset in regs_, 0 = no OTG found
    uint32_t            otgFrames_       {0};
    uint64_t            otgAbs_          {0};
    bool                probed_          {false};
    bool                fastWrites_      {false};   // GOP writes measured >= 2.5 GB/s (write-combined)
    bool                directKva_       {false};   // fKva points at the GOP itself: NVAccel writes the panel

    uint32_t            periodUs_        {6944};
    uint64_t            nextTickAbs_     {0};   // absolute time of the next steady-state tick
    uint64_t            lastSeq_         {0};
    uint32_t            testMode_        {0};
    uint32_t            holdTicks_       {0};
    uint32_t            targetPeriodUs_  {0};   // primehz-derived (default 60 Hz = 16666 us)
    uint32_t            idleTicks_       {0};   // consecutive unchanged ticks
    bool                lowFreqMode_     {false};

    // On-disk log sink (RAM ring buffer flushed to a text file by the tick loop)
    uint8_t            *logBuf_          {nullptr};
    size_t              logBufCap_       {0};
    size_t              logBufLen_       {0};
    uint32_t            logTick_         {0};
    char                logPath_[MAXPATHLEN] {};
    bool                logReady_        {false};
    bool                logEnabled_      {true};

    // Calibration mire (primecolors=1) state
    uint32_t            colorTest_        {0};
    uint32_t            colorStage_       {0};
    uint64_t            colorStageTicks_  {0};
};
