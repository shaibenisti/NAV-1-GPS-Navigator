// =============================================================================
//  BootReport  -  Prints chip / flash / PSRAM at boot so every run proves the
//  build used the correct FQBN (PSRAM must be 8 MB OPI, flash 16 MB).
// =============================================================================
#pragma once

// Returns false if the memory configuration doesn't match the verified board.
bool printBootReport();
