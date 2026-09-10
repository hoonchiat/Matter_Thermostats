/*
 * Temperature sensor: 10K Type-3 NTC on the SAADC -> Matter
 * TemperatureMeasurement::MeasuredValue (endpoint 1).
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

#include "temperature_sensor.h"

#include <zephyr/kernel.h>
#include <zephyr/drivers/adc.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/logging/log.h>

#include <platform/CHIPDeviceLayer.h>
#include <app-common/zap-generated/attributes/Accessors.h>

#include <cmath>

LOG_MODULE_REGISTER(temp_sensor, CONFIG_CHIP_APP_LOG_LEVEL);

using namespace chip;
using namespace chip::app::Clusters;

/* ---- Sensor / circuit constants (tune here) ----------------------------- */
namespace
{
constexpr EndpointId kTempEndpoint = 1;

/* 10K Type-3 NTC: 10 kOhm @ 25 C, Beta(25/85) ~= 3976 K.
 * Beta model is accurate to ~+/-0.5 C near room temperature; swap for an R-T
 * lookup table if you need the full Type-3 curve across a wide range. */
constexpr double kR0Ohms   = 10000.0; /* NTC resistance at T0 */
constexpr double kT0Kelvin = 298.15;  /* 25 C */
constexpr double kBeta     = 3976.0;  /* Type-3 */
constexpr double kRFixedOhms = 10000.0; /* lower divider resistor to GND */

constexpr int kAdcMaxCode = (1 << 12) - 1; /* 12-bit resolution */

constexpr uint32_t kSampleIntervalS = 30;
constexpr uint32_t kFirstSampleS = 5;
constexpr uint32_t kPowerSettleMs = 3;

/* Divider midpoint on AIN2 and the GPIO that powers the divider top. */
const struct adc_dt_spec sAdcTherm = ADC_DT_SPEC_GET_BY_IDX(DT_PATH(zephyr_user), 0);
const struct gpio_dt_spec sThermPwr =
	GPIO_DT_SPEC_GET(DT_PATH(zephyr_user), therm_pwr_gpios);
} /* namespace */

bool TemperatureSensorReadCenti(int16_t *centi)
{
	if (!device_is_ready(sAdcTherm.dev) || !gpio_is_ready_dt(&sThermPwr)) {
		return false;
	}

	/* Power the divider only for the measurement (battery life). */
	gpio_pin_set_dt(&sThermPwr, 1);
	k_msleep(kPowerSettleMs);

	int16_t raw = 0;
	struct adc_sequence seq = {
		.buffer = &raw,
		.buffer_size = sizeof(raw),
	};
	(void)adc_sequence_init_dt(&sAdcTherm, &seq);
	int err = adc_read_dt(&sAdcTherm, &seq);

	gpio_pin_set_dt(&sThermPwr, 0);

	if (err) {
		LOG_ERR("ADC read failed (%d)", err);
		return false;
	}
	if (raw <= 0 || raw >= kAdcMaxCode) {
		/* Open/short divider or sensor absent. */
		LOG_WRN("thermistor reading out of range (raw=%d) - sensor wired?", raw);
		return false;
	}

	/* Ratiometric: ratio = V(node)/VDD = raw / full-scale.
	 * Divider is PWR -> NTC -> node -> RFixed -> GND, so
	 *   ratio = RFixed / (Rntc + RFixed)  ->  Rntc = RFixed * (1 - ratio)/ratio. */
	const double ratio = static_cast<double>(raw) / static_cast<double>(kAdcMaxCode);
	const double rNtc = kRFixedOhms * (1.0 - ratio) / ratio;

	/* Beta equation: 1/T = 1/T0 + (1/Beta) * ln(Rntc / R0). */
	const double tKelvin = 1.0 / (1.0 / kT0Kelvin + (1.0 / kBeta) * std::log(rNtc / kR0Ohms));
	const double tCelsius = tKelvin - 273.15;

	*centi = static_cast<int16_t>(std::lround(tCelsius * 100.0));
	return true;
}

static void SampleWorkHandler(struct k_work *work)
{
	int16_t centi;
	if (!TemperatureSensorReadCenti(&centi)) {
		return;
	}

	chip::DeviceLayer::PlatformMgr().LockChipStack();
	TemperatureMeasurement::Attributes::MeasuredValue::Set(kTempEndpoint, centi);
	chip::DeviceLayer::PlatformMgr().UnlockChipStack();

	int whole = centi / 100;
	int frac = centi % 100;
	if (frac < 0) {
		frac = -frac;
	}
	LOG_INF("temperature = %d.%02d C", whole, frac);
}

K_WORK_DEFINE(sSampleWork, SampleWorkHandler);

static void SampleTimerHandler(struct k_timer *timer)
{
	k_work_submit(&sSampleWork);
}

K_TIMER_DEFINE(sSampleTimer, SampleTimerHandler, nullptr);

void TemperatureSensorInit(void)
{
	if (adc_channel_setup_dt(&sAdcTherm) != 0) {
		LOG_ERR("ADC channel setup failed");
	}
	if (gpio_is_ready_dt(&sThermPwr)) {
		gpio_pin_configure_dt(&sThermPwr, GPIO_OUTPUT_INACTIVE);
	}
	k_timer_start(&sSampleTimer, K_SECONDS(kFirstSampleS), K_SECONDS(kSampleIntervalS));
	LOG_INF("temperature sensor started (every %u s)", kSampleIntervalS);
}
