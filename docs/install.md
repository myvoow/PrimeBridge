# Installing PrimeBridge

[Back to the README](../README.md)

## Before you start

- An AMD iGPU from the [supported list](compatibility.md#supported-igpus), with the internal panel wired only to it.
- An NVIDIA GPU that NullMoth drives, with NullMoth 2.6.3 or later already working: a monitor plugged into the NVIDIA GPU shows an accelerated desktop.
- An HDMI port wired to the NVIDIA GPU, with nothing plugged into it at boot. That is the case on most gaming laptops: a monitor on that port shows NullMoth's desktop. A laptop without one cannot use PrimeBridge yet.
- OpenCore, with `Misc > Security > Vault` set to `Optional`.
- macOS 15 or 26, already booting normally (USB ports mapped and so on).
- The Xcode Command Line Tools (`xcode-select --install`) and internet access for the first build, which downloads about 170 MB (NullMoth's source and NVIDIA's open-gpu-kernel-modules headers).
- A 32 bpp firmware boot framebuffer inside the iGPU's BAR0. PrimeBridge checks this at boot and refuses otherwise.

> [!WARNING]
> PrimeBridge is a kernel extension. It writes to the iGPU's memory and installs patched builds of two NullMoth kexts. It comes with no warranty. Keep an HDMI monitor at hand for your first boots, and try it from a USB stick first if you can.

## Install

```bash
git clone https://github.com/myvoow/PrimeBridge.git
cd PrimeBridge
./install.sh
```

`install.sh` runs as your user and asks for your password before it changes anything. It:

1. Checks the Mac: macOS version, Command Line Tools, NullMoth version and that it runs, the NVIDIA GPU and the iGPU, the panel resolution and refresh rate, and OpenCore's EFI partition (from the boot path OpenCore records in NVRAM). `./install.sh --dry-run` shows what it finds and stops there.
2. Builds NullMoth's NVRMFB and NVAccel for the installed NullMoth version and macOS version, with the patch and a forced EDID made for the panel, then builds PrimeBridge. When it has the original kexts, it also checks that its unpatched builds match them byte for byte. A difference only means another compiler than NullMoth's, and the build is used anyway.
3. Edits OpenCore's `config.plist`, after saving a copy in `/Library/PrimeBridge`: adds PrimeBridge to `Kernel > Add`, adds the missing boot-args (`nvfb=1 nvaccel=1 nvfbforce=1 nvcursor=0 -nvrmnoflip`, plus `gfxrst=4` with WhateverGreen), removes `-primeoff`, and turns off the Fix PAT kernel patches ([why](how-it-works.md#why-fix-pat-must-be-off)).
4. Installs the patched NVRMFB and NVAccel, keeps NullMoth's originals in `/Library/PrimeBridge`, and rebuilds the auxiliary kernel collection.

Then approve the extensions in System Settings > Privacy & Security if macOS asks, unplug any monitor from the NVIDIA GPU, restart, and run `./doctor.sh`. The direct mode starts a few seconds after the first frame reaches the panel.

When detection fails, give it what it could not find: `--panel 1920x1080`, `--panel-hz 144` or `--efi disk0s1`.

`nvfb=1 nvaccel=1` belong to NullMoth, which needs its own boot-args too (see its docs). `nvfbforce=1` creates the fake HDMI monitor. `nvcursor=0` turns off the NVIDIA hardware cursor, so macOS draws the cursor into the frame and it shows on the panel.

## The forced refresh rate

The forced HDMI mode runs at a whole divisor of the panel's refresh rate (72 Hz on a 144 Hz panel). On a first install PrimeBridge has not measured the panel yet, so the mode is 60 Hz.

On iGPU ID `1002:1681` (680M / 660M), PrimeBridge measures the panel at runtime. After the restart, `./doctor.sh` says when running `./install.sh` again gives a better rate.

The other iGPUs cannot measure the panel, so give the rate yourself when the panel is not 60 Hz: `./install.sh --panel-hz 144`.

## After a NullMoth update

NullMoth's updater puts its own NVRMFB and NVAccel back. Until `./install.sh` runs again, PrimeBridge steps aside after 2 minutes and the panel gets macOS's basic display driver, without acceleration. A notification at login says so.

## Rollback

- `./uninstall.sh` puts NullMoth's original kexts back, removes PrimeBridge and the boot-args it added from OpenCore, and turns the Fix PAT patches it disabled back on. Restart afterwards. Every `config.plist` the scripts changed is saved in `/Library/PrimeBridge`.
- Black internal screen: add `-primeoff` to the boot-args to stop PrimeBridge. You can also plug a monitor into the NVIDIA GPU before booting: nothing is forced then, the monitor works as usual and the panel mirrors it. When no NVIDIA head comes up within 2 minutes, PrimeBridge steps aside by itself so macOS's basic display driver can take the panel.

Next: [check that it works](troubleshooting.md).
