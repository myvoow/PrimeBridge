#!/bin/sh
set -e
cd "$(dirname "$0")"
[ $# -gt 0 ] || set -- NVRMFB.kext NVAccel.kext
V=$(/usr/libexec/PlistBuddy -c 'Print :CFBundleVersion' /Library/Extensions/NVRM.kext/Contents/Info.plist)
for K in "$@"; do
    [ -d "$K" ] || { echo "no $K here"; exit 1; }
    KV=$(/usr/libexec/PlistBuddy -c 'Print :CFBundleVersion' "$K/Contents/Info.plist")
    [ "$V" = "$KV" ] || { echo "$K is from NullMoth $KV but NullMoth $V is installed. Not installing: use the kexts built for $V."; exit 1; }
done
for K in "$@"; do
    N=$(/usr/libexec/PlistBuddy -c 'Print :CFBundleExecutable' "$K/Contents/Info.plist")
    I=/Library/Extensions/$N.kext
    if [ ! -d "$N-$V-orig.kext" ]; then
        if LC_ALL=C grep -q -e nvfbforce -e 'panel by the copy engine' "$I/Contents/MacOS/$N"; then
            echo "The installed $N is already a patched one: no original kept in this folder."
        else
            cp -R "$I" "$N-$V-orig.kext"
        fi
    fi
    rm -rf "$I"
    cp -R "$K" "$I"
    chown -R root:wheel "$I"
    chmod -R 755 "$I"
done
kmutil create -n aux --allow-missing-kdk \
  -B /System/Library/KernelCollections/BootKernelExtensions.kc \
  -S /System/Library/KernelCollections/SystemKernelExtensions.kc \
  -A /Library/KernelCollections/AuxiliaryKernelExtensions.kc \
  -r /Library/Extensions
KC=/Library/KernelCollections/AuxiliaryKernelExtensions.kc
echo "in the collection: NVRMFB patch $(LC_ALL=C grep -a -c nvfbforce $KC), NVAccel patch $(LC_ALL=C grep -a -c 'panel by the copy engine' $KC) (0 = not patched)"
echo "Approve the update in System Settings > Privacy & Security if asked, then reboot."
