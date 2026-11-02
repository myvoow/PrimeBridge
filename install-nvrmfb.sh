#!/bin/sh
# Installs the patched NVRMFB.kext (NullMoth 2.6.3 + nvfbforce) from this folder into /Library/Extensions.
# Keeps your original as NVRMFB-2.6.3-orig.kext next to this script. Rollback: sudo sh install-nvrmfb.sh NVRMFB-2.6.3-orig.kext
set -e
cd "$(dirname "$0")"
K=${1:-NVRMFB.kext}
[ -d "$K" ] || { echo "no $K here"; exit 1; }
[ -d NVRMFB-2.6.3-orig.kext ] || cp -R /Library/Extensions/NVRMFB.kext NVRMFB-2.6.3-orig.kext
rm -rf /Library/Extensions/NVRMFB.kext
cp -R "$K" /Library/Extensions/NVRMFB.kext
chown -R root:wheel /Library/Extensions/NVRMFB.kext
chmod -R 755 /Library/Extensions/NVRMFB.kext
kmutil create -n aux --allow-missing-kdk \
  -B /System/Library/KernelCollections/BootKernelExtensions.kc \
  -S /System/Library/KernelCollections/SystemKernelExtensions.kc \
  -A /Library/KernelCollections/AuxiliaryKernelExtensions.kc \
  -r /Library/Extensions
echo "nvfbforce in the collection: $(LC_ALL=C grep -a -c nvfbforce /Library/KernelCollections/AuxiliaryKernelExtensions.kc) (0 = not patched)"
echo "Approve the update in System Settings > Privacy & Security if asked, then reboot."
