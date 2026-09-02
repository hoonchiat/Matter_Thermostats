/*
 * ble_scan.h - passive BLE scan for Matter devices currently in pairing mode.
 *
 * A commissionable Matter device advertises service 0xFFF6 carrying ONLY:
 *   discriminator (12-bit) + VendorID + ProductID + adv version.
 *
 * The setup PASSCODE is deliberately NOT advertised - it is the shared secret
 * that proves physical possession, and is printed on the device label / QR only.
 * So a scan can tell you WHICH device is in pairing mode and its discriminator,
 * but the passcode must still come from the label:
 *     pin <passcode> <discriminator>    then    pair
 *
 * Doubles as a diagnostic: if a device doesn't show up here, it isn't
 * advertising - which is usually the real cause of a "commissioning" failure
 * (FOUNDATION section 11), not the hub firmware.
 */
#pragma once

#include <cstdint>

/* One commissionable device seen during a scan. */
struct ble_scan_dev_t { uint16_t discriminator; uint16_t vid; uint16_t pid; };

/* Completion callback (invoked on the CHIP task): `devs`/`n` on success (n>=0),
 * or n<0 if the scan could not start (BLE busy/disabled). */
typedef void (*ble_scan_done_fn)(const ble_scan_dev_t *devs, int n);

/* Start a passive scan for `seconds` (clamped 1..60). If `on_done` is null the
 * results print to the console (classic behaviour); otherwise `on_done` is called
 * on completion with the collected devices and nothing is printed. */
void ble_scan_start_ex(uint16_t seconds, ble_scan_done_fn on_done);

/* Console shortcut: ble_scan_start_ex(seconds, nullptr). */
void ble_scan_start(uint16_t seconds);
