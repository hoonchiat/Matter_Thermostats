# Firmware

ESP-IDF v5.2+ / ESP-Matter project for the Matter Thread Thermostat.

## Build & flash

```bash
# One-time: install & export ESP-IDF and ESP-Matter
#   source $IDF_PATH/export.sh
#   source $ESP_MATTER_PATH/export.sh

idf.py set-target esp32c6
idf.py menuconfig        # Component config → "Matter Thermostat" (pins, SHT40, control)
idf.py build flash monitor
```

Development builds use Espressif's test attestation + the default setup code
(discriminator `0xF00`, passcode `20202021`). For production, provision a real
VID/PID and DAC/PAI into the `fctry` partition — see [../docs/MATTER.md](../docs/MATTER.md).

## Host unit tests (no hardware needed)

The pure components — the control law, occupancy, and the SHT40 conversions — plus the
OLED renderer compile and run natively:

```bash
cd test/host
make            # builds & runs: thermostat_core, occupancy, sht4x, ui
make preview    # render the OLED screens to the terminal as ASCII
```

These cover the hysteresis / min-off / min-on / startup-lockout / auto-dead-zone logic
(including the boot fail-safe); the vacancy timeout; the Sensirion CRC-8 + tick→°C/%RH
conversions; and the OLED render assertions (e.g. humidity on HOME). The same suite runs in
CI on every push and pull request — see [`../.github/workflows/ci.yml`](../.github/workflows/ci.yml).

## Layout

| Path | What |
|---|---|
| `main/` | app entry, Matter glue, control orchestration, NVS, Kconfig |
| `components/sht4x/` | SHT40 I²C temperature + humidity (driver + pure conversions) |
| `components/thermostat_core/` | pure control law (hysteresis + cycle protection) |
| `components/rotary_encoder/` | PCNT quadrature decoder |
| `components/button/` | debounced short/long-press |
| `components/relays/` | W/Y/G/O·B output driver |
| `components/occupancy/` | PIR sensor + vacancy timeout / manual Home-Away |
| `components/i18n/` | UI string catalog (English / French / Spanish / German) |
| `components/ui_oled/` | SSD1306/SH1106 screen manager + 5×7 font + settings menu |
| `test/host/` | host-side unit + UI tests for the pure components |

See [../docs/FIRMWARE.md](../docs/FIRMWARE.md) for the architecture and task model.
