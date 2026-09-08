# Firmware

ESP-IDF v5.2+ / ESP-Matter project for the Matter Thread Thermostat.

## Build & flash

```bash
# One-time: install & export ESP-IDF and ESP-Matter
#   source $IDF_PATH/export.sh
#   source $ESP_MATTER_PATH/export.sh

idf.py set-target esp32c6
idf.py menuconfig        # Component config → "Matter Thermostat" (pins, thermistor, control)
idf.py build flash monitor
```

Development builds use Espressif's test attestation + the default setup code
(discriminator `0xF00`, passcode `20202021`). For production, provision a real
VID/PID and DAC/PAI into the `fctry` partition — see [../docs/MATTER.md](../docs/MATTER.md).

## Host unit tests (no hardware needed)

The pure components — NTC conversion and the control law — compile and run natively:

```bash
cd test/host
make            # builds and runs both suites
```

These cover the R→T LUT interpolation and the hysteresis / min-off / min-on /
startup-lockout / auto-dead-zone logic, including the boot fail-safe.

## Layout

| Path | What |
|---|---|
| `main/` | app entry, Matter glue, control orchestration, NVS, Kconfig |
| `components/thermistor/` | 10K Type 2/3 NTC → °C (ADC + pure conversion) |
| `components/thermostat_core/` | pure control law (hysteresis + cycle protection) |
| `components/rotary_encoder/` | PCNT quadrature decoder |
| `components/button/` | debounced short/long-press |
| `components/relays/` | W/Y/G/O·B output driver |
| `components/ui_oled/` | SSD1306/SH1106 screen manager |
| `test/host/` | host-side unit tests for the pure components |

See [../docs/FIRMWARE.md](../docs/FIRMWARE.md) for the architecture and task model.
