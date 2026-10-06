// =============================================================================
//  version.h  -  Firmware version (single source of truth)
// -----------------------------------------------------------------------------
//  Semantic version, 0.x while in development:
//    0.0.x  platform/foundation baselines (M1-M2)
//    0.1.0  first usable OS-like version
//  Every released version gets a git tag "v<FW_VERSION>".
//
//  FW_GIT_DESCRIBE identifies the exact source: idf-build.ps1 writes it into
//  build_info.h (not versioned) from `git describe --tags --always --dirty`, e.g.
//    "v0.0.1"               built from the tagged commit
//    "v0.0.1-3-g1a2b3c4"    3 commits after the tag
//    "v0.0.1-dirty"         uncommitted changes in the build
// =============================================================================
#pragma once

#define FW_VERSION_MAJOR 0
#define FW_VERSION_MINOR 5
#define FW_VERSION_PATCH 2
#define FW_VERSION       "0.5.2"

#if __has_include("build_info.h")
#include "build_info.h"
#else
#define FW_GIT_DESCRIBE  "unknown (not built with build.ps1)"
#endif
