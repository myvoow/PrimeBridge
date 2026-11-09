# FAQ

[Back to the README](../README.md)

## Is my laptop supported?

You need an AMD iGPU from the [supported list](compatibility.md#supported-igpus), an NVIDIA GPU that NullMoth drives, and an HDMI port wired to the NVIDIA GPU.

`./install.sh --dry-run` checks the macOS version, the Command Line Tools, NullMoth, both GPUs, the panel and OpenCore's EFI partition, and changes nothing. It cannot see whether the NVIDIA GPU has an HDMI port. That shows up after the first boot, in `./doctor.sh`.

## Does every iGPU in the list get the same result?

All 12 PCI IDs install and use the same GOP framebuffer path. ID `1002:1681` (680M / 660M) also gets the copy engine direct mode and the panel refresh measurement. The other IDs go direct through BAR0 when writes are fast enough, and stay in staging mode otherwise. The [compatibility table](compatibility.md#supported-igpus) shows this per ID, and ID `1681` is the only one that has run on a laptop so far.

## `install.sh` says my iGPU "is not supported yet"

Its PCI ID is not in the list. Vega iGPUs are left out because NootedRed drives them, and Intel iGPUs are not matched yet. For another AMD iGPU, you can [add its ID](development.md#a-different-igpu) to `IOPCIPrimaryMatch` in `Info.plist`.

## My NVIDIA GPU has no HDMI port

PrimeBridge cannot use that laptop yet. It forces the first HDMI connector of the NVIDIA GPU to report a display, and DisplayPort needs a real sink to train the link. `./doctor.sh` says `this NVIDIA GPU has no HDMI port to force` in that case.

## Do I need a monitor plugged in?

No, leave the NVIDIA GPU's HDMI port empty at boot. With a monitor plugged in, nothing is forced: the monitor works as usual and the panel shows a copy of it in staging mode, which costs more CPU than direct mode.

## The internal screen stays black

Add `-primeoff` to the boot-args to stop PrimeBridge, or plug a monitor into the NVIDIA GPU before booting. When no NVIDIA head comes up within 2 minutes, PrimeBridge steps aside by itself so macOS's basic display driver can take the panel. The [rollback steps](install.md#rollback) cover the rest.

## NullMoth updated and I lost acceleration

NullMoth's updater puts its own NVRMFB and NVAccel back. Run `./install.sh` again. A notification at login reminds you until you do.

## Why 72 Hz on a 144 Hz panel?

The forced HDMI mode uses the highest whole divisor of the panel's rate that is at most 75 Hz and fits under 340 MHz. That gives 72 Hz on 144 Hz, 60 Hz on 120 Hz and 55 Hz on 165 Hz.

## My forced mode is stuck at 60 Hz

A first install has not measured the panel, so it uses 60 Hz. On ID `1681`, restart, then run `./install.sh` again when `./doctor.sh` tells you to. The other iGPUs cannot measure the panel, so run `./install.sh --panel-hz 144` with your panel's real rate.

## How do I know if I am in direct mode?

`./doctor.sh` prints the mode. `direct-copy-engine` and `direct-kva` are direct. A mode starting with `staging` is normal for a few seconds after boot. If it stays there, see [troubleshooting](troubleshooting.md#when-something-is-off).

## Why does `install.sh` turn off Fix PAT?

AMD Vanilla's Fix PAT patches make every write-combined mapping uncached, and writes to the iGPU took about 20 times longer on the test machine. [The measurements are here](how-it-works.md#why-fix-pat-must-be-off). `./uninstall.sh` turns the patches it disabled back on.

## I get short freezes or tearing

Both are [known limits](compatibility.md#known-limits). NVAccel can wait up to 100 ms for a composite to finish, which shows as a short freeze. Tearing is possible because the iGPU scans out the same buffer NVAccel writes into.

## How is battery life?

Limited. The NVIDIA GPU stays powered all the time since it renders the desktop.

## Do sleep and wake work?

They are untested.

## How do I remove it?

Run `./uninstall.sh`, then restart. It puts NullMoth's original kexts back and removes PrimeBridge and the boot-args it added. Every `config.plist` the scripts changed stays saved in `/Library/PrimeBridge`.
