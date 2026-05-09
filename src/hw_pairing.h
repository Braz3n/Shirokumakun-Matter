/*
 * Derive discriminator and SPAKE2+ passcode from the nRF52840 FICR hardware
 * device ID so every unit gets unique pairing credentials without factory data.
 */

#pragma once

#include <lib/core/CHIPError.h>

namespace HwPairing {

/* Compute credentials from FICR, build the SPAKE2+ verifier, and register a
 * CommissionableDataProvider.  Must be called before PrepareServer(). */
CHIP_ERROR Init();

} // namespace HwPairing
