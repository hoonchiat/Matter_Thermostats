/*
 * Per-device provisioning for the nRF52840 temperature sensor.
 *
 * Reads {discriminator, passcode, serial} from the `factory` flash partition and
 * feeds them to Matter through a custom CommissionableDataProvider. The SPAKE2+
 * verifier is computed on-device from the stored passcode + a fixed salt, so the
 * browser provisioning tool only has to write three small values (no crypto in
 * the browser) as a tiny UF2 that targets the factory partition. Falls back to
 * the compiled-in test defaults (20202021 / 3840) when the partition is blank.
 *
 * This mirrors the ESP32 light/button `provisioning.cpp` byte-for-byte, so the
 * one browser tool provisions every device family.
 */
#pragma once

#include <lib/core/CHIPError.h>
#include <cstdint>

/* Read the factory partition and register the custom CommissionableDataProvider.
 * Call from Nrf::Matter::InitData::mPreServerInitClbk, i.e. before Server::Init. */
CHIP_ERROR ProvisioningPreServerInit();

uint16_t    ProvisioningDiscriminator();
const char *ProvisioningSerial();
bool        ProvisioningIsProvisioned();
