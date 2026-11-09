# Checking and troubleshooting

[Back to the README](../README.md)

## Checking that it works

```bash
./doctor.sh
```

It prints PrimeBridge's mode and, when something is wrong, what to do about it. The details are in `/var/log/PrimeBridge.log`, started over at boot once it passes 8 MB.

```bash
tail -40 /var/log/PrimeBridge.log
ioreg -rc PrimeBridge -d1 | grep -E "Version|PrimeMode"
```

A healthy start on iGPU ID `1002:1681` (680M / 660M):

```
PrimeBridge 0.7.1 starting (log=1)
FB 1920x1080 bpp 32 stride 7680 B ... phys 0x490000000 ... BAR0 0x490000000 +0x10000000
found NVRMNVDAFramebuffer 1920x1080 pitch 7680 phys 0x...
NVAccel's present path armed after about 200 ms
redirected NVRMNVDAFramebuffer scanout kva to cached staging (8323072 bytes; NVRMFB allocation 8323072, GOP mapping 8323072)
igpu probe: FB_OFFSET 0x00000440 FB_LOCATION 0x0000f400..0x0000f41f -> VRAM at phys 0x440000000, 512 MB; GOP at VRAM +0x0
igpu probe: phys 0x440000000: 32 of 32 pixels written through BAR0 read back there -> MATCH
igpu writes: PAT 0x... (PA6 WC)
igpu writes: ns per row: BAR0 2004, BAR0 write-combined 2343, VRAM write-combined 1355 -> VRAM at phys, fast
NVAccel now puts its frames straight into the panel's buffer, with the GPU copy engine (no staging copy, no diff)
rate-hint: 4 Hz (idle), work avg 0 us ..., panel 143.78 Hz, direct
```

The other iGPUs have no register map in PrimeBridge, so their log differs in four places:

- The probe line is `igpu probe: device 0x15bf, no register map here, BAR0 write benchmark only`, with that iGPU's own ID.
- The `igpu writes: ns per row` line shows `VRAM write-combined -1`, because there is no VRAM address to try.
- In direct mode the switch line has no `with the GPU copy engine`, and `PrimeMode` is `direct-kva`.
- `rate-hint` lines say `panel 0.00 Hz`, because the refresh rate cannot be read.

## When something is off

- `PA6 not WC` with writes around 40 µs per row: the Fix PAT patch is still active.
- `no NVIDIA head after 60 s: ...` (`PrimeMode` is `no-nvidia-head`): NVRMFB did not create head 0, and the line says why. NullMoth's NVRM is not running, the NVIDIA GPU has no HDMI port to force, NVKMS refused the forced display, `nvfbforce=1` is missing, or the NVRMFB is not the one `install.sh` built (a NullMoth update put its own back): run `./install.sh` again. When the line says NVRMFB has a display but started no framebuffer, look at the `NVRM-fb` lines of the kernel log. After 2 minutes PrimeBridge steps aside (`still no NVIDIA head after 2 min`).
- `PrimeMode` is `no-nvaccel-present`: the NVIDIA head exists but NVAccel never armed its present path. Check `nvaccel=1`.
- `a display is plugged into the NVIDIA GPU`: nothing was forced at boot. That monitor works as usual and the panel shows a copy of it (its top-left 1920x1080 when it is larger), in staging mode, which costs more CPU than direct mode.
- No `direct` at the end of the `rate-hint` lines, with `fast` missing from the `igpu writes` line: the best mapping took 3 µs per row or more, so PrimeBridge stays in staging mode.
- No `direct` at the end of the `rate-hint` lines, with `fast` present: compare `NVRMFB allocation` and `GOP mapping` on the `redirected` line. If the allocation is larger, NVRMFB can switch to a mode larger than the panel's buffer, so PrimeBridge stays in staging mode to keep it from writing past that buffer.
- `NVRMFB reports a scanout allocation of 0 bytes`: this NVRMFB lacks the patch from this release (0.4.x, or NullMoth's own). PrimeBridge does not redirect and copies from NVIDIA memory instead, which is slow. Run `./install.sh` again.
- `this NVRMFB has no nvScanoutRedirect`: the NVRMFB was patched by an older release. Run `./install.sh` again.
- NullMoth's counters can help: `sysctl debug.nvaccel_iop_ok debug.nvaccel_surface_gpu_timeouts debug.nvaccel_iop_copy_skip`.

To stop PrimeBridge or go back to NullMoth alone, see [Rollback](install.md#rollback).

## Boot-args

| Boot-arg | Effect |
|---|---|
| `-primeoff` | PrimeBridge does not load (escape hatch) |
| `-primelogoff` | No log file |
| `primecarve=0` | Skip the write benchmark (stays on BAR0 as mapped at boot, no direct mode) |
| `primehz=N` | Staging timer rate (default 60, 24 to 240) |
| `primefmt=N` | Pixel packing in staging mode: 1 = BGRX (default), 2 = RGBX, 3 = raw. Direct mode runs only with 1 |
| `primemirror=1` | Copy each frame to a monitor plugged into the NVIDIA GPU too (staging mode only). On by itself when such a monitor was there at boot |
| `primedirect=1` | Skip the redirect and copy from NVIDIA memory, which is slow |
| `primetest=1/2/3`, `primecolors=1` | Test patterns on the panel during boot, for checking colors and stride |
| `primehold=N` | Seconds a `primetest` pattern stays up during boot (default 60, up to 600) |

## Sending a report

Open an issue with the [Test report](https://github.com/myvoow/PrimeBridge/issues/new?template=test-report.yml) template, or reach me on Discord: **@64417**.
