#pragma once
#include <IOKit/IOService.h>
#include <IOKit/IOWorkLoop.h>
#include <IOKit/IOTimerEventSource.h>
#include <IOKit/IOLocks.h>
#include <libkern/c++/OSSymbol.h>
#include <sys/param.h>
#include "BootFramebuffer.hpp"

struct NVRMVramRequest {
    uint32_t version;
    uint32_t flags;
    uint64_t size;
    uint64_t actualSize;
    uint64_t phys;
    void    *kva;
    void    *handle;
    uint64_t mappedTotal;
    uint32_t width;
    uint32_t height;
    uint32_t pitch;
    uint32_t _pad;
    uint64_t allocationCookie;
};
static_assert(sizeof(NVRMVramRequest) == 0x50, "NVRMVramRequest must be 0x50 bytes (matches nvrm_vram_abi.h)");

class PrimeBridge : public IOService {
    OSDeclareDefaultStructors(PrimeBridge)
public:
    IOService *probe(IOService *provider, SInt32 *score) override;
    bool start(IOService *provider) override;
    void stop(IOService *provider) override;
    void detach(IOService *provider) override;

    bool registerFrameSource(PrimeFrameSource *src);
    void unregisterFrameSource(PrimeFrameSource *src);
private:
    bool setup(IOService *provider);
    void cleanup();
    void copyTick(IOTimerEventSource *sender);
    void tickNullMoth(IOTimerEventSource *sender);
    bool findNvidiaFramebuffer();
    bool attachNvidiaScanout();
    bool redirect(void *kva, uint64_t phys);
    const void *ourKva() const;
    uint32_t tickUs() const;
    void probeIgpu();
    void chooseWriteMapping(uint64_t vramGop);
    void logf(const char *fmt, ...) __printflike(2, 3);
    void logFlush(bool sync = true);
    void logShutdown();

    IOPCIDevice        *igpu_            {nullptr};
    BootFramebuffer     fb_;
    IOWorkLoop         *wl_              {nullptr};
    IOTimerEventSource *timer_           {nullptr};
    IOLock             *lock_            {nullptr};
    IOLock             *logLock_         {nullptr};
    PrimeFrameSource   *source_          {nullptr};

    const OSSymbol     *symScanoutInfo_  {nullptr};
    const OSSymbol     *symRedirect_     {nullptr};
    IOService          *nvfb_            {nullptr};
    uint8_t            *origNvKva_       {nullptr};
    uint8_t            *staging_         {nullptr};
    uint8_t            *shadow_          {nullptr};
    size_t              stagingSize_     {0};
    uint64_t            nvPhys_          {0};
    uint64_t            nvSize_          {0};
    uint64_t            nvAlloc_         {0};
    uint32_t            nvWidth_         {0};
    uint32_t            nvHeight_        {0};
    uint32_t            nvPitch_         {0};
    bool                redirected_      {false};
    bool                firstFrame_      {true};
    uint32_t            mirrorExternal_  {0};
    uint32_t            directVram_      {0};
    uint32_t            pixelFmt_        {1};

    uint64_t            framesCopied_    {0};
    uint32_t            flipCleared_     {0};
    uint32_t            statTick_        {0};
    uint64_t            statWork_        {0};
    uint64_t            statWorkMax_     {0};
    uint64_t            statBlit_        {0};
    uint64_t            statRows_        {0};

    IOMemoryMap        *mmio_            {nullptr};
    volatile uint32_t  *regs_            {nullptr};
    uint32_t            otgFrameReg_     {0};
    uint32_t            otgFrames_       {0};
    uint64_t            otgAbs_          {0};
    bool                probed_          {false};
    bool                fastWrites_      {false};
    bool                directKva_       {false};
    uint64_t            directPhys_      {0};
    uint64_t            vramGop_         {0};
    uint32_t            benchTries_      {0};
    bool                rebench_         {false};
    int64_t             iopOk_           {-1};

    uint32_t            periodUs_        {6944};
    uint64_t            nextTickAbs_     {0};
    uint64_t            lastSeq_         {0};
    uint32_t            testMode_        {0};
    uint32_t            holdTicks_       {0};
    uint32_t            targetPeriodUs_  {0};
    uint32_t            idleTicks_       {0};
    uint32_t            waitTicks_       {0};
    bool                sawNvfb_         {false};
    bool                lowFreqMode_     {false};

    uint8_t            *logBuf_          {nullptr};
    size_t              logBufCap_       {0};
    size_t              logBufLen_       {0};
    uint32_t            logTick_         {0};
    bool                logReady_        {false};
    bool                logEnabled_      {true};

    uint32_t            colorTest_        {0};
    uint32_t            colorStage_       {0};
    uint64_t            colorStageTicks_  {0};
};
