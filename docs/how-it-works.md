# How it works

[Back to the README](../README.md)

## The problem

The internal panel is physically connected to the iGPU, which macOS cannot drive. NullMoth drives the NVIDIA GPU, but its accelerator (NVAccel) runs only when an NVIDIA display head exists, and that normally needs a monitor on the NVIDIA card. Meanwhile the firmware's GOP has already lit the panel and left the iGPU scanning out a plain framebuffer in its own memory (1920x1080, 32 bpp, BGRX on the test machine). Anything written there appears on the panel.

## The data path

1. With `nvfbforce=1`, NVRMFB makes the first HDMI connector of the NVIDIA GPU report itself connected when nothing is plugged in. Its EDID is named "PrimeBridge" and `install.sh` makes it for the panel: the panel's resolution, at a whole divisor of its refresh rate (1920x1080 at 72 Hz, 160.85 MHz on the test laptop). Head 0 exists, WindowServer composites on the NVIDIA GPU with Metal, and NVAccel runs. This works on HDMI only: DisplayPort needs a real sink to train the link.
2. Every frame, WindowServer's swap goes through `NVDisplayPipe::performTransaction` in NullMoth's `nvrm-accel.cpp`. With zero-copy flips off (`debug.nvaccel_iop_flip=0`, which PrimeBridge enforces), stock NVAccel pages the frame out of VRAM, then copies it with the CPU to the scanout address NVRMFB gives it (`fKva`).
3. PrimeBridge finds `NVRMNVDAFramebuffer` and calls `nvScanoutRedirect`, which the patch adds to NVRMFB. From then on `nvScanoutInfo` hands NVAccel PrimeBridge's buffer instead of NVRMFB's own.
4. PrimeBridge starts in staging mode. The scanout points to a cached RAM buffer the size of NVRMFB's whole scanout allocation (`fMemBytes`, which the patched NVRMFB reports), so a later, larger mode still fits. A 60 Hz timer (20 Hz when idle) compares it row by row with a shadow copy, only after NVAccel's frame counter (`debug.nvaccel_iop_ok`) has moved, and writes the changed rows to the GOP framebuffer.
5. On the first timer tick without a new frame, a few seconds after the first frame reaches the panel, PrimeBridge probes the iGPU.

   On ID `1681` (680M / 660M) it maps the register BAR (BAR5) and reads `GCMC_VM_FB_OFFSET` to find where the iGPU's VRAM sits in system RAM (`0x440000000`, 512 MB on the test machine), as Linux amdgpu does on x86 APUs. It writes 32 pixels through BAR0, reads them back from that RAM, puts the old values back within microseconds and logs `MATCH`, whatever is on screen. Then it times rewriting 64 identical rows (invisible on screen) through BAR0 as mapped at boot, BAR0 write-combined, and VRAM write-combined, and keeps the fastest mapping.

   The other IDs have no register map, so they time only the two BAR0 mappings. Only the 32 test pixels (ID `1681`), these timed rewrites and the final switch write anything.

   If writes take under 3 µs per row, the NVIDIA mode has exactly the panel's width, height and pitch, and NVRMFB's allocation fits in the panel's mapping, PrimeBridge points the scanout straight at the panel's framebuffer. The staging copy and the row diff drop out, and the PrimeBridge timer drops to 4 Hz to watch the geometry. The log says `direct`.

   When that framebuffer is the iGPU's VRAM in system RAM, which only ID `1681` can find, PrimeBridge also hands NVRMFB its physical address, and the patched NVAccel has the NVIDIA copy engine write each frame there by DMA, with no CPU copy at all. Frames whose newest pixels are not in VRAM still go through the CPU. `PrimeMode` becomes `direct-copy-engine`. When the panel's buffer is in BAR0, `PrimeMode` is `direct-kva` and NVAccel keeps copying with the CPU.
6. Every 64 timer ticks (every tick in direct mode) PrimeBridge re-reads the NVIDIA scanout geometry. If it changed (resolution switch), PrimeBridge asks NVRMFB to take its own buffer back, falls back to staging, and goes direct again once the geometry matches.
7. On ID `1681`, PrimeBridge also reads the OTG frame counter and logs the panel's real refresh rate on every `rate-hint` line.

PrimeBridge never writes to the VRAM address unless it first read back there exactly what it had written through BAR0.

## Why Fix PAT must be off

macOS maps write-combined (WC) memory through PAT entry PA6. AMD Vanilla's `_mtrr_update_action | Fix PAT` patches (Algrey's is on by default) replace macOS's PAT with `0x0007010600070106`, where PA6 is UC- (uncached). Every WC mapping in macOS then becomes uncached, PrimeBridge's writes and CPU access to NVIDIA VRAM included.

Measured on the test machine, per 7680-byte row:

| | Fix PAT on | Fix PAT off |
|---|---|---|
| BAR0 | 39.7 µs | 2.0 µs |
| iGPU VRAM, write-combined | 75.4 µs | 1.4 µs (about 5.7 GB/s) |
| Result | staging mode, `kernel_task` at 75 % in video | direct mode, `kernel_task` at 2.5 % |

AMD Vanilla's own README says the patch does not improve performance and that some systems run better without it. Test without it on a USB stick first.
