# Matter-over-Thread Thermostat (ESP32-C6)

An open, DIY-friendly smart thermostat built on the **Espressif ESP32-C6**, speaking
**Matter over Thread**. It reads room temperature from an industry-standard **10 kΩ
Type 2 / Type 3 NTC thermistor**, is controlled locally with a **rotary encoder + push
button**, shows status on a small **I²C OLED**, and switches conventional 24 VAC HVAC
loads (W / Y / G / O·B) through relays.

Because it implements the standard Matter **Thermostat** device type, it pairs and works
with Apple Home, Google Home, Amazon Alexa, Samsung SmartThings and Home Assistant — no
cloud, no vendor lock-in, all local control over a Thread mesh.

> **Status:** Specification + firmware. This repository is the *engineering spec* for the
> device plus a structured ESP-IDF / ESP-Matter project. The self-contained logic
> (thermistor conversion, control/hysteresis, encoder & button input), the OLED UI
> (5×7 font, home/adjust/menu/info/pairing/fault screens) and the local **settings menu**
> (select NTC Type 2/3, toggle °C/°F, view the Matter pairing code) are implemented; the
> Matter endpoint wiring is laid out with clearly marked integration points against the
> installed SDK version. See [`docs/SPECIFICATION.md`](docs/SPECIFICATION.md).

---

## At a glance

| | |
|---|---|
| **MCU** | ESP32-C6 (RISC-V, native 802.15.4 for Thread, Wi-Fi 6, BLE 5) |
| **Connectivity** | Matter 1.x over Thread; BLE for commissioning |
| **Device type** | Matter Thermostat (`0x0301`) |
| **Temperature sensor** | 10 kΩ NTC, HVAC **Type 2** or **Type 3** curve (selectable), voltage-divider + ADC |
| **Display** | 0.96″ (nom. "0.95″") **SSD1306** 128×64 monochrome OLED, I²C |
| **Local input** | Incremental **rotary encoder** (quadrature + integrated switch) and a dedicated **push button** |
| **HVAC output** | 4 relay/SSR channels: W (heat), Y (cool/compressor), G (fan), O·B (reversing valve) |
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
| [`hardware/pinout.csv`](hardware/pinout.csv) | Machine-readable pin assignment |
| [`hardware/bom.csv`](hardware/bom.csv) | Bill of materials |

## Repository layout

```
Matter_Thermostats/
├── docs/                     # The specification (start with SPECIFICATION.md)
├── hardware/                 # Pinout + BOM (CSV)
├── tools/                    # NTC lookup-table generator
└── firmware/                 # ESP-IDF / ESP-Matter project
    ├── main/                 # App entry, Matter wiring, control glue
    └── components/
        ├── thermistor/       # 10K Type 2/3 NTC → °C  (implemented)
        ├── rotary_encoder/   # PCNT quadrature decoder + switch (implemented)
        ├── button/           # debounced short/long-press (implemented)
        ├── thermostat_core/  # hysteresis + cycle-protection control (implemented)
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

## License

Apache-2.0 — see [`LICENSE`](LICENSE). (Matches the licensing of ESP-IDF and ESP-Matter.)
