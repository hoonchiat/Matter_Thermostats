/* sht4x_logic.c — pure CRC + tick conversions (no hardware, host-testable). */
#include "sht4x.h"

uint8_t sht4x_crc8(const uint8_t *data, int len)
{
    uint8_t crc = 0xFF;
    for (int i = 0; i < len; ++i) {
        crc ^= data[i];
        for (int b = 0; b < 8; ++b) {
            crc = (crc & 0x80) ? (uint8_t)((crc << 1) ^ 0x31) : (uint8_t)(crc << 1);
        }
    }
    return crc;
}

float sht4x_ticks_to_c(uint16_t t_ticks)
{
    return -45.0f + 175.0f * ((float)t_ticks / 65535.0f);
}

float sht4x_ticks_to_rh(uint16_t rh_ticks)
{
    float rh = -6.0f + 125.0f * ((float)rh_ticks / 65535.0f);
    if (rh < 0.0f)   rh = 0.0f;
    if (rh > 100.0f) rh = 100.0f;
    return rh;
}
