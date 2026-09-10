/*
 * Temperature sensor for the Matter temperature-measurement endpoint.
 *
 * Reads a 10K Type-3 NTC thermistor in a resistor divider through the
 * nRF52840 SAADC (ratiometric against VDD) and drives the Matter
 * TemperatureMeasurement::MeasuredValue attribute on endpoint 1.
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

/* Start periodic sampling; posts each reading to the Matter attribute.
 * Call once after the Matter server has started. */
void TemperatureSensorInit(void);

/* Take one blocking reading. Returns true and *centi = temperature in
 * hundredths of a degree Celsius (Matter units), or false on error. */
bool TemperatureSensorReadCenti(int16_t *centi);
