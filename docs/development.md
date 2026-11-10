# Development

[Back to the README](../README.md)

## Adapting it to another laptop

### Panel resolution

Direct mode needs the NVIDIA mode to equal the GOP framebuffer: same width, height and pitch (width x 4). `install.sh` makes the forced EDID from the panel's resolution (`NVRMFB-patch/gen_edid.py`, CVT reduced blanking v2), and the pitch matches for the usual widths. Above 165 MHz the EDID gets a CTA-861 block that declares HDMI up to 340 MHz, and a mode that would need more gets a lower divisor of the panel's rate (a 60 Hz 4K panel gets 30 Hz). Only 1920x1080 has run on a laptop so far.

### A different iGPU

The GOP framebuffer path is the same on every iGPU PrimeBridge matches. Only the optional VRAM probe and the panel refresh readout use registers, taken from the Linux amdgpu headers for Rembrandt (`yellow_carp_offset.h`, `gc_10_3_0_offset.h`, `dcn_3_1_2_offset.h`), so they run on ID `1681` only. The other iGPUs time BAR0 writes instead, and go direct when BAR0 write-combined is fast enough. For an AMD iGPU that is not listed, add its ID to `IOPCIPrimaryMatch` in `Info.plist`.

### Another NullMoth version

PrimeBridge does not touch NVRMFB's private fields: it calls `nvScanoutRedirect` and reads `nvScanoutInfo`, both answered by the patched NVRMFB. `nvScanoutInfo` also reports the scanout allocation (`fMemBytes`) and, once the scanout is redirected to memory with a physical address, flags it and gives that address to NVAccel. NVRMFB also publishes two properties on NVRM: `nvfbforce-display`, the forced display or 0, and `nvfbforce`, what the force pass did (`forced`, `monitor`, `no-hdmi`, `refused` or `off`).

`install.sh` rebuilds NVRMFB and NVAccel for whatever NullMoth version is installed. It clones that NullMoth tag, and NVIDIA's open-gpu-kernel-modules at the release the tag is built on, into `~/Library/Caches/PrimeBridge`. The release comes from `NV_XNU_FW_DIR` in NullMoth's `kexts/NVRM/os-xnu2.cpp`. NullMoth's docs lag behind: 2.8.1's docs say 615.78.08 and its code uses 620.70.06. The same `nvfbforce.patch` applies to every tag from v2.6.3 to v2.8.1, and `install.sh` stops on a version it does not apply to.

To build by hand:

```bash
PANEL=1920x1080 PANELHZ=144 sh NVRMFB-patch/build-nvrmfb.sh 2.8.1 [stock NVRMFB binary] [stock NVAccel binary]
```

NullMoth builds a different NVAccel for macOS 15 and 26: the script builds for the Mac it runs on, and `MACOS_MAJOR=15` or `26` picks the other one. `WORK=` and `OGKM=` point it to existing checkouts, `OGKM_TAG=` overrides the NVIDIA release, and `EXTRA_PATCH=` applies one more patch after `nvfbforce.patch`. The result is in `nullmoth-<version>/patched`, and `sudo sh install-nvrmfb.sh <NVRMFB.kext> <NVAccel.kext>` installs it.

## Building PrimeBridge

On a Mac with the Command Line Tools (Xcode is not needed):

```bash
cd PrimeBridge-source
sh build.sh
```

`build.sh` uses the SDK `xcrun --show-sdk-path` reports; set `SDK=` to use another one. The output is `build/PrimeBridge.kext`, ad-hoc signed. Kernel code must not use floating point, so this has to print 0:

```bash
otool -tV build/PrimeBridge.kext/Contents/MacOS/PrimeBridge | grep -cE "cvtsi2sd|divsd"
```

## Tests

```bash
python3 -I test_primebridge.py
PANEL=1920x1080 PANELHZ=144 sh NVRMFB-patch/build-nvrmfb.sh 2.8.1
```

`test_primebridge.py` checks the installer's edits to `nvrm610.conf`. `build-nvrmfb.sh` runs `test_nvfbforce.py` on the patched source and on the generated EDID, which it also checks on a set of panels from 1366x768 to 4K.

GitHub Actions (`.github/workflows/build.yml`) runs both and builds the kext at every push, on NullMoth 2.6.3, 2.7.2 and 2.8.1 for macOS 15 and 26. Every Monday it also tries the newest NullMoth release, so a release the patch does not apply to shows up as a failed run.

## Where to start in the code

- `PrimeBridge.cpp`: `attachNvidiaScanout()` does the redirect, `probeIgpu()` and `chooseWriteMapping()` probe the iGPU and benchmark the writes, and `tickNullMoth()` runs the staging copy, the geometry check and the switch to direct mode.
- `BootFramebuffer.cpp` maps the GOP framebuffer (`attach()`, `mapWriteCombined()`).
- `NVRMFB-patch/nvfbforce.patch` is the NVIDIA side: the forced HDMI display and `nvScanoutRedirect` in NVRMFB, and `nvAccelIopToPanel`, the copy engine path of `NVDisplayPipe::performTransaction`, in NVAccel.

Diagnose from `/var/log/PrimeBridge.log` and `ioreg -rc PrimeBridge` before changing code. [`llms.txt`](../llms.txt) holds the full technical notes, for people and AI assistants working on the code.

## Repository contents

| Path | What it is |
|---|---|
| `install.sh`, `doctor.sh`, `uninstall.sh` | Install, check and remove PrimeBridge. All three run `primebridge.py` |
| `primebridge.py` | Detection, OpenCore editing and the install state in `/Library/PrimeBridge` |
| `PrimeBridge-source/` | Driver sources and `build.sh` |
| `NVRMFB-patch/nvfbforce.patch` | The change to NullMoth's NVRMFB (`nvrm-fb.cpp`) and NVAccel (`nvrm-accel.cpp`), for tags v2.6.3 to v2.8.1 |
| `NVRMFB-patch/build-nvrmfb.sh`, `gen_edid.py`, `test_nvfbforce.py` | Rebuild NVRMFB and NVAccel with the patch for a NullMoth version, make the forced EDID, and check the patch |
| `test_primebridge.py`, `.github/workflows/build.yml` | Installer tests, and the CI that builds every supported NullMoth version |
| `NVRMFB-patch/NullMoth-LICENSE`, `NullMoth-NOTICE` | NullMoth's license and notice |
| `install-nvrmfb.sh` | Installs the patched `NVRMFB.kext` and `NVAccel.kext`, keeps your originals, rebuilds the auxiliary kernel collection |
| `docs/` | Install guide, troubleshooting, FAQ, compatibility, internals and development notes |
| `llms.txt` | Full technical notes in one file |
