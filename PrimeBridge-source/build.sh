#!/bin/sh
# Builds PrimeBridge.kext with the Command Line Tools (no Xcode needed).
set -e
cd "$(dirname "$0")"
SDK=${SDK:-/Library/Developer/CommandLineTools/SDKs/MacOSX15.4.sdk}
K=$SDK/System/Library/Frameworks/Kernel.framework
OUT=build/PrimeBridge.kext
rm -rf build; mkdir -p $OUT/Contents/MacOS

CF="-target x86_64-apple-macos10.15 -isysroot $SDK -mkernel -fapple-kext -fno-builtin -fno-rtti -fno-exceptions \
 -nostdinc -nostdinc++ -I$K/Headers -I$K/Headers/libkern -DKERNEL -DKERNEL_PRIVATE -DDRIVER_PRIVATE -DAPPLE -DNeXT \
 -O2 -Wall -Wno-unused-parameter -Wno-inconsistent-missing-override"
clang++ -std=gnu++17 $CF -c src/BootFramebuffer.cpp -o build/BootFramebuffer.o
clang++ -std=gnu++17 $CF -c src/PrimeBridge.cpp -o build/PrimeBridge.o
clang   $CF -c src/kmod_info.c -o build/kmod_info.o

clang++ -target x86_64-apple-macos10.15 -isysroot $SDK -nostdlib -Xlinker -kext \
  build/BootFramebuffer.o build/PrimeBridge.o build/kmod_info.o \
  -L$SDK/usr/lib -lkmodc++ -lkmod -o $OUT/Contents/MacOS/PrimeBridge
cp Info.plist $OUT/Contents/Info.plist
codesign -f -s - $OUT
mkdir -p dist
rm -rf dist/PrimeBridge.kext
cp -R $OUT dist/PrimeBridge.kext
file $OUT/Contents/MacOS/PrimeBridge
