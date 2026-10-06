// =============================================================================
//  GpsParserSelfTest  -  Boot-time check of GpsParser against known sentences
// -----------------------------------------------------------------------------
//  Feeds synthetic NEO-8M-style sentences (a "no fix" set, then a "fix" set)
//  into a private GpsParser and checks every GpsData field. Proves the fix
//  path while indoors. Prints PASS/FAIL per check; returns overall result.
// =============================================================================
#pragma once

bool runGpsParserSelfTest();
