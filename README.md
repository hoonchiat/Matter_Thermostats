# Matter-over-Thread Thermostat (ESP32-C6)

[![CI](https://github.com/hoonchiat/Matter_Thermostats/actions/workflows/ci.yml/badge.svg)](https://github.com/hoonchiat/Matter_Thermostats/actions/workflows/ci.yml)
[![License: Apache-2.0](https://img.shields.io/badge/License-Apache_2.0-blue.svg)](LICENSE)

An open, DIY-friendly smart thermostat built on the **Espressif ESP32-C6**, speaking
**Matter over Thread**. It reads room temperature from an industry-standard **10 kΩ
Type 2 / Type 3 NTC thermistor**, is controlled locally with a **rotary encoder + push
button**, shows status on a small **I²C OLED**, and switches conventional 24 VAC HVAC
loads (W / Y / G / O·B) through relays.

Because it implements the standard Matter **Thermostat** device type, it pairs and works
with Apple Home, Google Home, Amazon Alexa, Samsung SmartThings and Home Assistant — no
cloud, no vendor lock-in, all local control over a Thread mesh.

> **Status:** Specification + firmware. This repository is the *engineering spec* for the
> device plus a structured ESP-IDF / ESP-Matter project. Implemented: the self-contained
> logic (thermistor conversion, control/hysteresis with fan-speed levels, encoder & button
> input), a modern OLED UI (5×7 font, status bar with mode/fan/link icons, rounded setpoint
> pill; home/adjust/menu/info/pairing/fault screens — previewable on a host PC), the local
> **settings menu** (fan Auto/Low/Med/High, Home/Away presence + source, NTC Type 2/3, °C/°F,
> Matter pairing code), **occupancy** (PIR sensor or manual toggle; Matter OCC feature with
> separate unoccupied setpoints), and full **remote override from Matter** of mode, setpoints
> and fan speed. Commissioning mirrors the esp-matter `light` example. The Matter endpoint wiring
> is laid out with clearly marked integration points against the installed SDK version. See
> [`docs/SPECIFICATION.md`](docs/SPECIFICATION.md).

---

## At a glance

| | |
|---|---|
| **MCU** | ESP32-C6 (RISC-V, native 802.15.4 for Thread, Wi-Fi 6, BLE 5) |
| **Connectivity** | Matter 1.x over Thread (self-healing mesh; mains-powered → acts as a Thread **router**/range extender); BLE for commissioning |
| **Device type** | Matter Thermostat (`0x0301`) |
| **Room sensor** | Selectable: **10 kΩ NTC** (HVAC Type 2/3, ADC) **or** **SHT40** I²C (temperature **+ humidity**) |
| **Humidity** | With SHT40: relative humidity on the OLED and as a Matter Humidity Sensor |
| **Display** | 0.96″ (nom. "0.95″") **SSD1306** 128×64 monochrome OLED, I²C |
| **Local input** | Incremental **rotary encoder** (quadrature + integrated switch), a dedicated **push button**, and a dedicated **fan-speed button** |
| **HVAC output** | Relay/SSR: W (heat), Y (cool/compressor), G (fan), O·B (reversing valve) + optional 3-tap multi-speed blower |
| **Fan control** | Auto / Low / Med / High — dedicated button, settings menu, and remote (Matter Fan Control cluster) |
| **Occupancy** | PIR/occupancy sensor **or** manual Home/Away toggle; Matter OCC feature with separate unoccupied setpoints; published as a Matter Occupancy Sensor |
| **Modes** | Off / Heat / Cool / Auto / Fan-only (Honeywell-Home-style UX) |
| **Power** | USB-C 5 V (bench) or 24 VAC → 5 V (field install) |
| **Framework** | ESP-IDF v5.x + ESP-Matter |

## Documentation

| Doc | What's in it |
|---|---|
| [`docs/SPECIFICATION.md`](docs/SPECIFICATION.md) | Product requirements, system architecture, and the complete component breakdown |
| [`docs/HARDWARE.md`](docs/HARDWARE.md) | Block diagram, **pin map**, thermistor front-end design & math, power, BOM |
| [`docs/FIRMWARE.md`](docs/FIRMWARE.md) | Firmware architecture, RTOS tasks, control algorithm & state machine |
| [`docs/MATTER.md`](docs/MATTER.md) | Matter data model (endpoints, clusters, attributes), commissioning flow |
| [`docs/UI.md`](docs/UI.md) | OLED screen layouts and the encoder/button interaction model |
| [`docs/USER_GUIDE.html`](docs/USER_GUIDE.html) | Owner's guide with every OLED screen rendered pixel-for-pixel |
| [`hardware/pinout.csv`](hardware/pinout.csv) | Machine-readable pin assignment |
| [`hardware/bom.csv`](hardware/bom.csv) | Bill of materials |

## Repository layout

```
Matter_Thermostats/
├── .github/workflows/        # CI — host tests on every push / PR
├── docs/                     # Spec + owner's guide (start with SPECIFICATION.md)
├── hardware/                 # Pinout + BOM (CSV)
├── tools/                    # NTC LUT, font, and user-guide generators
└── firmware/                 # ESP-IDF / ESP-Matter project
    ├── main/                 # App entry, Matter wiring, control glue
    ├── test/host/            # Host unit + UI tests (pure C, no hardware)
    └── components/
        ├── thermistor/       # 10K Type 2/3 NTC → °C  (implemented)
        ├── sht4x/            # SHT40 I2C temp + humidity (implemented)
        ├── rotary_encoder/   # PCNT quadrature decoder + switch (implemented)
        ├── button/           # debounced short/long-press (implemented)
        ├── relays/           # W/Y/G/O·B HVAC output driver (implemented)
        ├── thermostat_core/  # hysteresis + cycle-protection control (implemented)
        ├── occupancy/        # PIR sensor + vacancy timeout / manual Home-Away (implemented)
        └── ui_oled/          # SSD1306 screen manager + 5x7 font + settings menu (implemented)
```

## Quick start (firmware)

Prerequisites: [ESP-IDF v5.2+](https://docs.espressif.com/projects/esp-idf/) and
[ESP-Matter](https://docs.espressif.com/projects/esp-matter/) installed and exported.

```bash
cd firmware
idf.py set-target esp32c6
idf.py menuconfig          # Component config → Matter Thermostat  (pins, thermistor type…)
idf.py build flash monitor
```

Then commission the device with any Matter controller by scanning the QR code shown on
the OLED (or the pairing code printed on the console). A **Thread Border Router** must be
present on the network (Apple TV/HomePod, Google Nest Hub 2nd gen, or an
OpenThread Border Router such as Home Assistant + a Thread radio).

See [`docs/MATTER.md`](docs/MATTER.md) for the full commissioning walkthrough.

## Development & testing

The self-contained logic is covered by **host unit tests** that compile and run
natively — no ESP-IDF, no hardware:

```bash
cd firmware/test/host
make            # thermistor, control law, occupancy, SHT40, and OLED UI render tests
make preview    # print the OLED screens to the terminal as ASCII
```

Every push and pull request runs the same suite in **GitHub Actions**
([`.github/workflows/ci.yml`](.github/workflows/ci.yml)). A full ESP-IDF/ESP-Matter
build is intentionally left out of CI: the esp-matter toolchain is many gigabytes and
slow to provision, which makes it a poor fit for gating checks.

The owner-facing [`docs/USER_GUIDE.html`](docs/USER_GUIDE.html) is generated straight
from the firmware's own display code, so its screenshots are pixel-accurate:

```bash
firmware/test/host/render_screens | python3 tools/gen_userguide.py
```

## License

Apache-2.0 — see [`LICENSE`](LICENSE). (Matches the licensing of ESP-IDF and ESP-Matter.)
