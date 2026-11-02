#pragma once
// Single source of truth for the PrimeBridge version. Included by both the C++
// sources and kmod_info.c (C). Info.plist's CFBundleVersion must be kept in sync
// manually — build.sh does not inject it.
#define PRIMEBRIDGE_VERSION "0.4.8"
