# Product & Engineering Specification

**Project:** Matter-over-Thread Thermostat (ESP32-C6)
**Document status:** Baseline v0.1
**Last updated:** 2026-09-08

---

## 1. Overview

A wall-mount / bench smart thermostat that measures room temperature with a standard
HVAC-grade 10 kΩ NTC thermistor, regulates a conventional 24 VAC heating/cooling system,
and exposes itself to the smart-home ecosystem as a standard **Matter Thermostat** over a
**Thread** mesh network. All control is local; the cloud is never in the loop.

### 1.1 Goals

1. **Standards-based:** Implement the Matter 1.x Thermostat device type so it works with
   *any* Matter controller (Apple Home, Google Home, Alexa, SmartThings, Home Assistant).
2. **Thread-native:** Use the ESP32-C6's built-in 802.15.4 radio; join an existing Thread
   network, low-power and resilient mesh, BLE only for commissioning.
3. **Accurate sensing:** Support the two dominant HVAC thermistor curves — **10 kΩ
   Type 2** and **10 kΩ Type 3** — with datasheet-grade conversion accuracy (±0.3 °C over
   the comfort range after calibration).
4. **Great local UX:** Full standalone operation without a phone — rotary encoder to set
   temperature, push button for mode/menu, OLED for status.
5. **Real HVAC control:** Drive conventional relays/SSRs (W/Y/G/O·B) with proper
   hysteresis and compressor short-cycle protection.
6. **Hackable & documented:** Clean component boundaries, testable logic, open license.

### 1.2 Non-goals (v1)

- Multi-stage (W1/W2, Y1/Y2) staging — single stage per mode in v1 (hooks left in place).
- Built-in scheduling UI — scheduling is delegated to the Matter controller; on-device
  7-day scheduling is a future enhancement.
- Line-voltage (120/240 VAC) switching — this design targets 24 VAC low-voltage systems.
- Humidity / IAQ sensing (reserved I²C address space and endpoint noted for future).

---

## 2. Assumptions & clarifications

The prompt specified an *"OLED 0.95″ I²C"*, a *"digital rotary switch"*, a *"push
button"*, and a *"10K Type 2/3"* room sensor. The concrete engineering interpretation
used throughout this spec:

| Prompt term | Interpreted as | Rationale |
|---|---|---|
| OLED 0.95″ I²C | **SSD1306 128×64 monochrome OLED**, I²C (0.96″ is the ubiquitous part; 0.91″ 128×32 is a drop-in with a smaller layout) | 0.95″/0.96″ mono OLEDs are I²C; the true 0.95″ *color* part (SSD1331) is **SPI-only** and would change the pin map — see [HARDWARE](HARDWARE.md#oled-options). SH1106 is firmware-selectable. |
| Digital rotary switch | **Incremental quadrature rotary encoder** (e.g. Bourns/ALPS EC11) with detents and an **integrated momentary push switch** | "Digital" ⇒ incremental A/B encoder (not an analog pot, not an absolute encoder). |
| Push button | A **dedicated momentary push button**, separate from the encoder's shaft switch | Two distinct inputs give a cleaner UX (encoder-press = select, button = mode/back). |
| 10K Type 2/3 | **10 kΩ NTC thermistor**, HVAC **Type II or Type III** R-T curve, runtime-selectable | These are the two most common North-American HVAC thermistor curves. See [§7](#7-temperature-sensing). |

If the true intent was a **color 0.95″ SSD1331** display, only the display transport
(SPI instead of I²C) and three pins change; the rest of the design is unaffected.

---

## 3. System architecture

```
                 ┌──────────────────────────────────────────────────┐
                 │                    ESP32-C6                        │
   10K NTC ─┬────┤ ADC1_CH1  ── Thermistor front-end (divider+filter)│
  (Type2/3) │    │                                                   │
        Rfix│    │ I2C0 (SDA/SCL) ── SSD1306 OLED 128x64             │
            │    │                                                   │
           GND   │ PCNT ─────────── Rotary encoder A/B               │
                 │ GPIO ─────────── Encoder switch + Push button     │
                 │                                                   │
                 │ GPIO ×4 ──────── Relay drivers  W / Y / G / O·B ──┼──▶ 24VAC HVAC
                 │ RMT  ─────────── Status RGB LED (WS2812)          │
                 │                                                   │
                 │ 802.15.4 radio ─ Thread mesh  ◀── Border Router  │
                 │ BLE ──────────── Commissioning only              │
                 └──────────────────────────────────────────────────┘
```

**Software stack (bottom → top):**

```
   Application  ┌ app_main: orchestration, event routing, persistence (NVS)
                ├ thermostat_core: mode/hysteresis/cycle-timer control law
                ├ ui_oled: screen state machine & rendering
   Domain       ├ matter data-model glue (endpoint/cluster ↔ app state)
   Drivers      ├ thermistor · rotary_encoder · button · relays · status LED
   Middleware   ├ ESP-Matter (Data Model, Interaction Model)
                ├ CHIP / connectedhomeip stack
   Network      ├ OpenThread (802.15.4) · mDNS/SRP · BLE (commissioning)
   RTOS/HAL     └ ESP-IDF (FreeRTOS, ADC, I2C, PCNT, RMT, GPIO, NVS)
```

See [FIRMWARE.md](FIRMWARE.md) for tasks, queues, and the control algorithm, and
[MATTER.md](MATTER.md) for the exact data model.

---

## 4. Functional requirements

| ID | Requirement |
|---|---|
| FR-1 | Measure room temperature from a 10 kΩ Type 2 **or** Type 3 NTC, selectable at build/runtime, and report it as the Matter `LocalTemperature` attribute in 0.01 °C units. |
| FR-2 | Provide heating and cooling setpoints, adjustable locally (encoder) and remotely (Matter), within configurable min/max limits. |
| FR-3 | Support System Modes: **Off, Heat, Cool, Auto** (and expose Fan-Only). |
| FR-4 | Drive HVAC outputs with configurable **hysteresis (deadband)** and **minimum on/off cycle timers** to protect the compressor. |
| FR-5 | Display current temperature, setpoint, mode, call-for-heat/cool status, and network status on the OLED. |
| FR-6 | Commission over BLE and operate over Thread; rejoin automatically after power loss. |
| FR-7 | Persist user settings (setpoints, mode, units, calibration, deadband, thermistor type) across reboots in NVS. |
| FR-8 | Support °C/°F display (Matter `TemperatureDisplayMode`); internal math is always °C. |
| FR-9 | Provide a local **factory reset** (decommission) via long-press, with on-screen confirmation. |
| FR-10 | Show the Matter commissioning QR/pairing code on the OLED while uncommissioned. |
| FR-11 | Apply a user temperature-**calibration offset** (± a few °C) to correct sensor placement error. |
| FR-12 | Fail safe: on sensor fault (open/short) or lost logic power, drive all HVAC relays **de-energized (off)** and indicate a fault. |
| FR-13 | Provide **fan speed** control — Auto / Low / Med / High — adjustable locally (settings menu) and remotely (Matter Fan Control cluster). Auto runs the fan with the call; a fixed speed circulates continuously. |
| FR-14 | Allow **remote override** from Matter of System Mode, heating/cooling setpoints, and fan speed; local and remote state stay synchronized and overrides persist. |
| FR-15 | Present a modern, legible OLED UI (status bar with mode/fan/link indicators, large temperature, setpoint pill), taking cues from the Honeywell Home thermostats. |
| FR-16 | Support **occupancy** via a PIR/occupancy sensor (with a vacancy timeout) **or** a manual Home/Away toggle (selectable). Use the Matter Thermostat **OCC feature** with separate **unoccupied setpoints** (writable remotely and locally); publish the resolved presence via the `Occupancy` attribute and an Occupancy Sensor endpoint. |

## 5. Non-functional requirements

| ID | Requirement |
|---|---|
| NFR-1 | Temperature accuracy ±0.3 °C, 10–40 °C, after one-point calibration. |
| NFR-2 | Setpoint control resolution 0.5 °C (0.5 °F in °F mode). |
| NFR-3 | Local UI latency < 100 ms from input to on-screen feedback. |
| NFR-4 | Relay switching: enforce ≥ configurable **min-off** (default 300 s) and **min-on** (default 120 s) for compressor circuits. |
| NFR-5 | Recover Thread connectivity within 60 s of a router/power event without user action. |
| NFR-6 | Flash/RAM budget: fit Matter + Thread + app in a 4 MB flash part (8 MB recommended for OTA A/B). |
| NFR-7 | All safety-relevant defaults are conservative (outputs off) on any undefined/fault state. |

---

## 6. Hardware summary

Full detail in [HARDWARE.md](HARDWARE.md). Headlines:

- **MCU module:** ESP32-C6-WROOM-1 (or -1U w/ ext. antenna). 8 MB flash recommended.
- **Sensor front-end:** 10 kΩ NTC in a divider with a **10 kΩ 0.1 % reference resistor**,
  RC low-pass to the ADC, series/ESD protection. ADC read with curve-fit calibration.
- **Display:** SSD1306 128×64 I²C @ 0x3C.
- **Input:** EC11 rotary encoder (A/B/SW) + one momentary push button; BOOT button reused
  for factory reset.
- **Output:** 4× relay or SSR channels with flyback/snubber, opto-isolation recommended
  for 24 VAC.
- **Power:** USB-C (5 V) for bench; on-board 24 VAC→5 V (isolated) + 3.3 V rail for field.
- **Status:** on-board addressable RGB LED (WS2812) for at-a-glance state.

## 7. Temperature sensing

The device supports the two common North-American HVAC 10 kΩ NTC curves:

- **10 kΩ Type 2 ("10K-2")** — nominal β₍25/85₎ ≈ 3891 K
- **10 kΩ Type 3 ("10K-3")** — nominal β₍25/85₎ ≈ 3976 K

> ⚠️ **Type 2 and Type 3 are *not* interchangeable.** They share R₂₅ = 10 kΩ but diverge
> substantially away from 25 °C. Using the wrong curve produces errors of several degrees.
> The β values above are nominal; **the authoritative source is always the sensor
> manufacturer's R-T table.** This design therefore uses a **piecewise-linear lookup
> table (LUT)** as the primary conversion method, generated per curve from the datasheet
> (see [`tools/gen_ntc_lut.py`](../tools/gen_ntc_lut.py)), with a Steinhart-Hart / β model
> as a fallback and for interpolation.

Conversion pipeline (implemented in `components/thermistor`):

```
raw ADC → (median-of-N) → mV (ADC calibration) → R_ntc = R_fix · V/(Vref−V)
        → LUT interpolation (per Type) → °C → EMA smoothing → + user offset → LocalTemperature
```

Fault detection: an ADC reading pinned at the rails implies an open or shorted sensor →
report fault, force outputs off (FR-12). Math, divider topology, and accuracy budget are
in [HARDWARE.md §Thermistor](HARDWARE.md#thermistor-front-end).

## 8. Control behavior

Implemented in `components/thermostat_core` (pure, unit-testable). Summary:

- **Heat call:** in Heat/Auto, energize W when `T ≤ heatSet − deadband/2`; de-energize at
  `T ≥ heatSet + deadband/2`.
- **Cool call:** in Cool/Auto, energize Y (+G) when `T ≥ coolSet + deadband/2`;
  de-energize at `T ≤ coolSet − deadband/2`.
- **Auto:** honor both setpoints with an enforced **minimum dead-zone** between heat and
  cool setpoints to prevent fighting.
- **Fan (G):** speed is Auto/Low/Med/High. Auto runs the fan at the configured call speed
  while heating/cooling; Low/Med/High circulate continuously at that level. Single-`G`
  installs treat any non-Auto speed as on; multi-tap blowers energize one of
  `G_LOW/G_MED/G_HIGH`.
- **Reversing valve (O·B):** driven per heat-pump config in Cool (O) or Heat (B).
- **Occupancy (Home/Away):** from a PIR sensor (with vacancy timeout) or a manual toggle.
  Uses the Matter **OCC feature** — separate occupied and **unoccupied** setpoints; the
  control law runs on whichever set matches the current presence.
- **Protection:** min-on / min-off timers, especially compressor min-off (anti
  short-cycle); startup lockout after power-up.

Full state table and pseudocode in [FIRMWARE.md §Control](FIRMWARE.md#control-algorithm).

## 9. Matter data model (summary)

- **Endpoint 0** — Root Node: Basic Information, Network Commissioning (Thread), General
  Commissioning, OTA Requestor, etc.
- **Endpoint 1** — Thermostat (`0x0301`):
  - `Identify` (0x0003)
  - `Thermostat` (0x0201) — features **HEAT | COOL | AUTO | OCC**; attributes
    `LocalTemperature`, `Occupancy`, `OccupiedHeatingSetpoint`, `OccupiedCoolingSetpoint`,
    `UnoccupiedHeatingSetpoint`, `UnoccupiedCoolingSetpoint`, `SystemMode`,
    `ControlSequenceOfOperation`, setpoint limits, `ThermostatRunningState`.
  - `Thermostat User Interface Configuration` (0x0204) — `TemperatureDisplayMode`,
    `KeypadLockout`.
- **Endpoint 2** — Fan (`0x002B`):
  - `Fan Control` (0x0202) — `FanMode` (Off/Low/Med/High/Auto), `FanModeSequence`,
    `PercentSetting` — the fan speed, viewable and overridable from Matter.
- **Endpoint 3** — Occupancy Sensor (`0x0107`):
  - `Occupancy Sensing` (0x0406) — `Occupancy` bit reflecting the resolved Home/Away state.

All three of System Mode, setpoints, and fan speed can be **overridden remotely** and stay
in sync with the local UI. Full attribute/command list, ranges, and the local↔Matter
synchronization rules are in [MATTER.md](MATTER.md).

## 10. Commissioning & networking

- Uncommissioned: device advertises over BLE; OLED shows the QR/pairing code.
- Controller commissions over BLE, pushes the **Thread operational dataset**, device joins
  the Thread mesh (requires a **Thread Border Router** on the LAN).
- Operational: mDNS/SRP registration, Interaction Model over Thread. BLE is dropped.
- Reset: local long-press factory reset removes fabrics and Thread credentials.

## 11. Persistence

NVS namespace `thermo_cfg` stores: `sysMode`, `heatSet`, `coolSet`, `deadband`,
`tempOffset`, `units`, `ntcType`, `minOff`, `minOn`, `hpReversing`, `brightness`, `fan`
(fan speed), `occSrc`/`occHome` (occupancy source + manual Home/Away), and `uHeat`/`uCool`
(unoccupied setpoints). Matter's own fabric/credential storage is separate (managed by the stack).

## 12. Bill of materials

See [`hardware/bom.csv`](../hardware/bom.csv).

## 13. Open items / future work

- Multi-stage heat/cool (W2/Y2) and dehumidify.
- On-device 7-day schedule + Matter schedule attributes.
- Humidity sensor (SHT4x on the free I²C bus) → add Relative Humidity Measurement cluster.
- OTA firmware updates (A/B partitions already reserved).
- Power-stealing front-end for C-wire-less installs.

## 14. Glossary

| Term | Meaning |
|---|---|
| **Matter** | Application-layer smart-home interoperability standard (CSA). |
| **Thread** | Low-power IPv6 802.15.4 mesh networking. |
| **Border Router** | Bridges the Thread mesh to your Wi-Fi/Ethernet LAN. |
| **NTC** | Negative-Temperature-Coefficient thermistor. |
| **Deadband / hysteresis** | Temperature gap between turn-on and turn-off to avoid chatter. |
| **Short-cycling** | Rapid compressor on/off; damaging — prevented by min-off timers. |
| **W / Y / G / O·B** | HVAC control wires: heat / cool(compressor) / fan / reversing valve. |
