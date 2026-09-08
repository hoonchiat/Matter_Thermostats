/*
 * CHIP project configuration overrides for the Matter Thread Thermostat.
 * Referenced by CONFIG_CHIP_PROJECT_CONFIG in sdkconfig.defaults.
 *
 * Keep this minimal; most settings come from menuconfig. For PRODUCTION, set a
 * CSA-allocated Vendor ID and a real Product ID here (and provision DAC/PAI into
 * the 'fctry' partition), instead of relying on the development test values.
 */
#pragma once

/* Development identifiers — REPLACE for production. 0xFFF1 is a CSA test VID. */
#ifndef CHIP_DEVICE_CONFIG_DEVICE_VENDOR_ID
#define CHIP_DEVICE_CONFIG_DEVICE_VENDOR_ID 0xFFF1
#endif

#ifndef CHIP_DEVICE_CONFIG_DEVICE_PRODUCT_ID
#define CHIP_DEVICE_CONFIG_DEVICE_PRODUCT_ID 0x8001
#endif

#ifndef CHIP_DEVICE_CONFIG_DEVICE_PRODUCT_NAME
#define CHIP_DEVICE_CONFIG_DEVICE_PRODUCT_NAME "Matter Thread Thermostat"
#endif

/* This is a Thermostat device; a modest number of fabrics is plenty. */
#ifndef CHIP_CONFIG_MAX_FABRICS
#define CHIP_CONFIG_MAX_FABRICS 5
#endif
