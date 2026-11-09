#!/bin/sh
set -e
V=${1:?usage: sh build-nvrmfb.sh <NullMoth version> [stock NVRMFB binary] [stock NVAccel binary]}
STOCK=$2
STOCKA=$3
P=$(cd "$(dirname "$0")" && pwd)
W=${WORK:-$PWD/nullmoth-$V}
PANEL=${PANEL:-1920x1080}
PANELHZ=${PANELHZ:-60}
MAJOR=${MACOS_MAJOR:-$(sw_vers -productVersion | cut -d. -f1)}
[ "$MAJOR" -ge 26 ] && TAHOE=NM_TAHOE=1 || TAHOE=
[ -e "$W/.git" ] || git -c advice.detachedHead=false clone -q --depth 1 -b "v$V" https://github.com/nullmoth/nvidia-macos-driver.git "$W"
W=$(cd "$W" && pwd)
[ "$(git -C "$W" describe --tags --exact-match 2>/dev/null)" = "v$V" ] || { echo "$W is not a NullMoth v$V checkout: remove it or set WORK="; exit 1; }
git -C "$W" checkout -q -- kexts
NEED=${OGKM_TAG:-$(sed -n 's|^#define NV_XNU_FW_DIR "/Library/NullMoth/nvfw/nvidia/\([0-9.]*\)/"|\1|p' "$W/kexts/NVRM/os-xnu2.cpp")}
[ -n "$NEED" ] || { echo "cannot find which NVIDIA release NullMoth $V is built on (set OGKM_TAG)"; exit 1; }
OGKM=${OGKM:-$HOME/Library/Caches/PrimeBridge/ogkm-$NEED}
[ -f "$OGKM/version.mk" ] || git -c advice.detachedHead=false clone -q --depth 1 -b "$NEED" https://github.com/NVIDIA/open-gpu-kernel-modules.git "$OGKM"
HAVE=$(sed -n 's/^NVIDIA_VERSION = //p' "$OGKM/version.mk")
[ "$NEED" = "$HAVE" ] || { echo "NullMoth $V is built on NVIDIA $NEED, $OGKM has $HAVE: remove it or set OGKM="; exit 1; }
G=$OGKM/src
KH=$(xcrun --show-sdk-path)/System/Library/Frameworks/Kernel.framework/Headers

build() {
    K=$1/NVRMFB.kext; rm -rf "$1"; mkdir -p "$K/Contents/MacOS"
    xcrun clang++ -arch x86_64 -fapple-kext -mkernel -nostdinc -I"$KH" -I"$W/kexts/NVRMFB" -I"$W/kexts/NVRMFB/fb" \
        -I"$G/nvidia-modeset/os-interface/include" -I"$G/nvidia-modeset/kapi/interface" -I"$G/nvidia-modeset/interface" \
        -I"$G/common/sdk/nvidia/inc" -I"$G/common/unix/common/inc" -I"$G/common/inc" -I"$G/nvidia/arch/nvalloc/unix/include" \
        -DKERNEL -DKERNEL_PRIVATE -DDRIVER_PRIVATE -DAPPLE -DNeXT -mmacosx-version-min=15.0 -std=c++17 \
        -fno-rtti -fno-exceptions -fno-builtin -fno-common -O2 -w -c "$W/kexts/NVRMFB/fb/nvrm-fb.cpp" -o "$1/nvrm-fb.o"
    xcrun clang++ -arch x86_64 -mmacosx-version-min=15.0 -fapple-kext -nostdlib -Xlinker -kext -lkmodc++ -lkmod -lcc_kext \
        "$1/nvrm-fb.o" -o "$K/Contents/MacOS/NVRMFB"
    rm "$1/nvrm-fb.o"
    cp "$W/kexts/NVRMFB/fb/Info.plist" "$K/Contents/Info.plist"
    /usr/libexec/PlistBuddy -c "Set :CFBundleVersion $V" -c "Set :CFBundleShortVersionString $V" "$K/Contents/Info.plist"
    sed 's/^RMDEFS=().*/RMDEFS=(-DNVCPU_X86_64 -DNV_MACOSX)/' "$W/build/accel_build.sh" > "$1/accel_build.sh"
    /usr/bin/env -u NM_TAHOE OGKM="$OGKM" $TAHOE bash "$1/accel_build.sh" "$W/kexts/NVRM" "$1/accel" >/dev/null || { grep error "$1/accel/accel.err" | head; exit 1; }
    mv "$1/accel/NVAccel.kext" "$1/NVAccel.kext"; rm -rf "$1/accel" "$1/accel_build.sh"
    /usr/libexec/PlistBuddy -c "Set :CFBundleVersion $V" -c "Set :CFBundleShortVersionString $V" "$1/NVAccel.kext/Contents/Info.plist"
}

same() {
    [ -z "$2" ] || if cmp -s "$W/stock/$1.kext/Contents/MacOS/$1" "$2"; then echo "unpatched $1: byte-identical to $2"
    else echo "note: the unpatched $1 differs from $2 (another compiler or SDK than NullMoth's release); the patched build is used anyway"; fi
}

if [ -n "$STOCK$STOCKA" ]; then
    build "$W/stock"
    same NVRMFB "$STOCK"
    same NVAccel "$STOCKA"
fi
git -C "$W" apply "$P/nvfbforce.patch" || { echo "the PrimeBridge patch does not apply to NullMoth $V: this version is not supported yet"; exit 1; }
[ -z "$EXTRA_PATCH" ] || git -C "$W" apply "$EXTRA_PATCH" || { echo "EXTRA_PATCH $EXTRA_PATCH does not apply to NullMoth $V"; exit 1; }
python3 -I "$P/gen_edid.py" "$PANEL" "$PANELHZ" > "$W/kexts/NVRMFB/fb/nvfbforce_edid.inc"
PANEL=$PANEL PANELHZ=$PANELHZ python3 -I "$P/test_nvfbforce.py" "$W"
build "$W/patched"
echo "built $W/patched for macOS $MAJOR: NVRMFB md5 $(md5 -q "$W/patched/NVRMFB.kext/Contents/MacOS/NVRMFB"), NVAccel md5 $(md5 -q "$W/patched/NVAccel.kext/Contents/MacOS/NVAccel")"
