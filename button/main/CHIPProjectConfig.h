/*
 * CHIP project config for the button.
 * Overrides the Basic Information identity the device reports over Matter. Included by
 * CHIPDeviceConfig.h (via CONFIG_CHIP_PROJECT_CONFIG) BEFORE its #ifndef defaults, so
 * these win. Cosmetic identity only - the VID/PID (0xFFF1/0x8001, set in sdkconfig) and
 * the test attestation (DAC/PAI/CD) are unchanged, so commissioning still works as-is.
 * NOTE: some phone apps display a vendor/product name from the CSA certification database
 * keyed by VID/PID, not from the device, so on the test VID 0xFFF1 they may still show a
 * generic label; the hub + the Basic Information cluster report the names below correctly.
 */
#pragma once

#define CHIP_DEVICE_CONFIG_DEVICE_VENDOR_NAME              "Daikin PJoshua"
#define CHIP_DEVICE_CONFIG_DEVICE_PRODUCT_NAME             "PJoshua Button"
#define CHIP_DEVICE_CONFIG_DEFAULT_DEVICE_HARDWARE_VERSION        0
#define CHIP_DEVICE_CONFIG_DEFAULT_DEVICE_HARDWARE_VERSION_STRING "0.1"
/* SoftwareVersionString is NOT set here: esp-matter's DeviceInstanceInfoProvider returns
 * the ESP-IDF app version, so it comes from CONFIG_APP_PROJECT_VER="0.99" (sdkconfig).
 * SerialNumber is added explicitly in app_main (create_serial_number "PJB-A01-0001") -
 * this macro is only the provider fallback. */
#define CHIP_DEVICE_CONFIG_TEST_SERIAL_NUMBER              "PJB-A01-0001"
