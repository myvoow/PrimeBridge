# Compatibility

[Back to the README](../README.md)

## Supported iGPUs

PrimeBridge supports these AMD iGPUs. `install.sh` accepts them and the driver attaches to them, from the `IOPCIPrimaryMatch` list in `PrimeBridge-source/Info.plist`.

| Radeon iGPU | PCI IDs (vendor `1002`) | Copy engine direct mode | Panel Hz measured |
|---|---|---|---|
| Vega (Ryzen 2000 to 5000) | `15dd`, `15d8`, `1636`, `1638`, `164c`, `15e7` | Yes | Yes |
| 680M / 660M | `1681`, `164d` | Yes | Yes |
| 780M / 760M / 740M | `15bf`, `15c8`, `1900`, `1901` | No | Yes |
| 610M | `164e`, `13c0`, `1506` | No | Yes |
| 890M / 880M | `150e` | No | No |
| 860M / 840M / 820M | `1114`, `1902` | No | No |
| 8060S / 8050S | `1586` | No | No |

These two features read iGPU registers, whose addresses come from the Linux amdgpu headers. A feature is on only where those headers give both the register and the base address of its block. On the last three rows Linux reads that base address from the chip at run time, so PrimeBridge leaves the features off there.

The GOP framebuffer path is the same on all of them. Without the copy engine, PrimeBridge still goes direct (`direct-kva`) when write-combined writes to BAR0 take under 3 µs per row, and stays in staging mode otherwise.

Only ID `1681` has run on a laptop so far. Reports from the other IDs are the most useful ones: see [Feedback](../README.md#feedback).

On a Vega iGPU, PrimeBridge and NootedRed cannot run together, since both take the iGPU: `install.sh` stops when NootedRed is loaded. Intel iGPUs are not matched yet.

## Laptops that have run it

| | First laptop | Second laptop | Third laptop |
|---|---|---|---|
| Model | Acer Nitro V 15 ANV15-41 | | Acer Nitro V 15 ANV15-41-R1JY |
| CPU | Ryzen 5 7535HS | Ryzen 7 6800H | |
| iGPU ID | `1002:1681` | | |
| NVIDIA GPU | GeForce RTX 4050 Laptop | RTX 3050 | |
| Panel | 1920x1080, 144 Hz | | |
| Software | macOS 26.7.2, NullMoth 2.6.3 | | |
| PrimeBridge | 0.7.1 | 0.4.8 | |
| Result | Direct mode with the copy engine, forced mode 72 Hz | Works correctly, has not tried 0.7.1 yet | |

The patch also builds for NullMoth 2.7.2 and 2.8.1, for macOS 15 and 26, but those have not run on a laptop yet.

## Known limits

- PrimeBridge needs an HDMI port on the NVIDIA GPU. Laptops whose NVIDIA GPU has only DisplayPort or USB-C outputs, or none, are not supported yet.
- The fake HDMI monitor runs at a divisor of the panel's rate (72 Hz on a 144 Hz panel), so each frame stays up for the same number of refreshes. The two clocks are independent, so every few seconds one frame shows for one refresh more or less.
- NVAccel waits up to 100 ms for a composite to finish before copying (`kSurfGpuWaitMs` in `nvrm-accel.cpp`). The test machine still hits a few of these per 10 minutes (`debug.nvaccel_surface_gpu_timeouts`), seen as short freezes.
- Tearing is possible, because the iGPU scans out the same buffer NVAccel writes into (no double buffering).
- Sleep and wake are untested.
- NullMoth's opt-in AMD and Intel test driver (NMDRM) attaches to the same iGPU and has not been tried with PrimeBridge.
- A smaller mode picked in System Settings shows top-left without scaling.
- The NVIDIA GPU stays powered all the time since it renders the desktop, so battery life is limited.
- Only 1920x1080 has run on a laptop so far.
