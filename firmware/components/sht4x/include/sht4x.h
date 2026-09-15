/*
 * sht4x — Sensirion SHT40/SHT41/SHT45 I2C temperature + humidity sensor.
 *
 * Shares the OLED's I2C bus (default address 0x44). The CRC-8 check and the
 * tick->engineering-unit conversions are pure functions (host-testable); the
 * measurement transaction lives in the ESP driver.
 */
#ifndef SHT4X_H
#define SHT4X_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---- PURE helpers (no hardware) ------------------------------------------ */

/* Sensirion CRC-8: poly 0x31, init 0xFF, no reflection, final XOR 0x00. */
uint8_t sht4x_crc8(const uint8_t *data, int len);

/* Raw 16-bit ticks -> temperature (°C):  T = -45 + 175 * ticks / 65535. */
float sht4x_ticks_to_c(uint16_t t_ticks);

/* Raw 16-bit ticks -> relative humidity (%RH), clamped to 0..100:
 *   RH = -6 + 125 * ticks / 65535. */
float sht4x_ticks_to_rh(uint16_t rh_ticks);

/* ---- ESP driver ---------------------------------------------------------- */

/* Attach the SHT4x as a device on an already-created I2C master bus.
 * `i2c_bus` is an i2c_master_bus_handle_t (opaque here to stay host-friendly). */
int  sht4x_init(void *i2c_bus, uint8_t addr);

/*
 * Trigger a high-precision measurement and read temperature (°C) and humidity
 * (%RH). Returns ESP_OK on a completed measurement; sets *fault on an I2C or
 * CRC error (values then invalid).
 */
int  sht4x_read(float *temp_c, float *rh_pct, bool *fault);

#ifdef __cplusplus
}
#endif

#endif /* SHT4X_H */
