# Hardware Design

Covers the block diagram, **pin assignment**, the SHT40 room sensor, input/output stages,
power, and the BOM. Machine-readable copies live in
[`hardware/pinout.csv`](../hardware/pinout.csv) and [`hardware/bom.csv`](../hardware/bom.csv).

---

## 1. Block diagram

```
                       ┌─────────────────────────────────────┐
   USB-C 5V ───▶ 3V3   │                                     │
   24VAC ─▶ [iso buck]─▶ LDO/buck ─▶ 3V3 ─▶ ESP32-C6-WROOM-1  │
                       │                                     │
   SHT40 (temp+RH) ◀── I2C0 0x44 (shared bus) ──────────────│
   OLED SSD1306 ◀──── I2C0 (SDA GPIO6 / SCL GPIO7) ─────────│
   Encoder A/B ─────────────────────────▶ PCNT (GPIO10/11)  │
   Encoder SW ──────────────────────────▶ GPIO2             │
   Push button ─────────────────────────▶ GPIO3             │
   Fan speed btn ───────────────────────▶ GPIO14            │
   RGB status LED ◀──── RMT (GPIO8)                          │
   Relays W/Y/G/OB ◀── GPIO18/19/20/21 ─▶ [drivers] ─▶ 24VAC │
   BOOT/reset btn ──────────────────────▶ GPIO9             │
                       └─────────────────────────────────────┘
```

---

## 2. Pin assignment (ESP32-C6-WROOM-1)

The ESP32-C6 GPIO matrix is flexible; the assignment below avoids the **strapping pins**
(GPIO4, 5, 8*, 9*, 15), the **USB-Serial-JTAG** pins (GPIO12/13), the **UART0 console**
(GPIO16/17), and the internal SPI-flash pins (GPIO24–30).

| Signal | GPIO | On-chip peripheral | Direction | Notes |
|---|---|---|---|---|
| **I²C SDA** (OLED + SHT40) | GPIO6 | I2C0 | I/O | 4.7 kΩ pull-up to 3V3; shared bus |
| **I²C SCL** (OLED + SHT40) | GPIO7 | I2C0 | O | 4.7 kΩ pull-up to 3V3; shared bus |
| **Encoder A / CLK** | GPIO10 | PCNT ch0 | IN | Hardware quadrature decode |
| **Encoder B / DT** | GPIO11 | PCNT ch0 | IN | Hardware quadrature decode |
| **Encoder switch** | GPIO2 | GPIO (ISR) | IN | Internal pull-up; press = select |
| **Push button** | GPIO3 | GPIO (ISR) | IN | Internal pull-up; mode / back |
| **Fan-speed button** | GPIO14 | GPIO (ISR) | IN | Internal pull-up; cycles Auto/Low/Med/High (−1 = unused) |
| **Relay W** (heat) | GPIO18 | GPIO | OUT | Active-high to driver |
| **Relay Y** (cool/compressor) | GPIO19 | GPIO | OUT | Active-high to driver |
| **Relay G** (fan enable) | GPIO20 | GPIO | OUT | Active-high; any fan speed > 0 |
| **Relay O·B** (reversing valve) | GPIO21 | GPIO | OUT | Heat-pump only |
| **Fan taps** G_LOW/MED/HIGH (opt.) | −1 | GPIO | OUT | Multi-speed blower; one-hot; disabled (−1) by default |
| **Occupancy / PIR** (opt.) | −1 | GPIO | IN | Motion input; disabled (−1) = manual Home/Away only |
| **Status RGB LED** | GPIO8* | RMT (WS2812) | OUT | On-board on DevKitC-1 (strapping — LED only) |
| **Factory-reset button** | GPIO9* | GPIO | IN | Re-uses BOOT (strapping, pulled-up) |
| **Console UART TX/RX** | GPIO16/17 | UART0 | — | Debug/log; keep free |
| **USB D−/D+** | GPIO12/13 | USB-Serial-JTAG | — | Flash/monitor; keep free |

`*` GPIO8 and GPIO9 are strapping pins; both are used here only in roles that tolerate it
(a WS2812 output that is high-Z at reset, and a button that is externally pulled to the
level BOOT expects). Do not repurpose them for peripherals that drive them at boot.

These names are mirrored 1:1 in `firmware/main/Kconfig.projbuild` so every pin is a
menuconfig option — change the board without touching code.

---

## 3. Room sensor (SHT40)

The room sensor is a **Sensirion SHT40** — a digital, factory-calibrated temperature +
relative-humidity sensor on I²C. It is the only room sensor: there is no analog front-end,
no ADC, and no thermistor.

| Part | Interface | Provides | Address | Notes |
|---|---|---|---|---|
| **SHT40** (SHT40-AD1B) | I²C | temperature **+ relative humidity** | 0x44 (0x45 for -BD1B) | shares the OLED bus; no extra MCU pins |

### 3.1 Wiring

The SHT40 is a 4-pin part (VDD / GND / SDA / SCL) that sits on the **same I²C0 bus** as the
OLED (SDA GPIO6, SCL GPIO7, shared 4.7 kΩ pull-ups). The firmware creates one I²C master bus
and adds both the OLED (0x3C) and the SHT40 (0x44) to it, so adding the sensor costs **no
extra GPIO**. Decouple VDD with 100 nF close to the part.

```
   3V3 ──┬───────────────┐
         │             [ SHT40 ]
      [0.1µF]   SDA ──── GPIO6 (I2C0, shared with OLED)
         │      SCL ──── GPIO7 (I2C0, shared with OLED)
        GND ───── GND
```

### 3.2 Reading & conversion

The `sht4x` component issues the **high-precision measure** command (`0xFD`), reads 6 bytes
(temperature word + CRC, humidity word + CRC), validates both bytes with the Sensirion
**CRC-8** (poly 0x31, init 0xFF), and converts the raw ticks:

```
T  [°C] = −45 + 175 · S_T  / 65535
RH [%]  =  −6 + 125 · S_RH / 65535     (clamped to 0…100 %)
```

A user **calibration offset** (`offset`, 0.01 °C) is added after conversion. These pure
functions are host-unit-tested (`firmware/test/host/test_sht4x.c`), including the datasheet
CRC vector `0xBEEF → 0x92`.

### 3.3 Accuracy & fault handling

- **Datasheet accuracy:** ±0.2 °C (typ.) temperature, ±1.8 %RH (typ.) — comfortably within
  the ±0.3 °C comfort-band target (NFR-1), with no board-level calibration required.
- **Fault:** a failed read or CRC mismatch ⇒ report fault, blank the Matter
  `LocalTemperature`, and force all relays off (FR-12).
- **Self-heating:** negligible at the ~1 Hz sampling used here (single-shot high-precision
  reads, sensor idle between samples).

---

## 4. OLED display

- **Default:** SSD1306, 128×64, monochrome, I²C, address **0x3C** (0x3D selectable).
- **Alternates:** SH1106 128×64 (firmware flag), SSD1306 128×32 0.91″ (reduced layout).
- Shared **I²C0 bus @ 400 kHz** — the OLED (0x3C) and the optional **SHT40** (0x44) sit on
  the same two wires. The firmware creates one I²C master bus and adds both devices to it.

### OLED options

| Part | Size | Interface | Controller | Use here |
|---|---|---|---|---|
| 0.96″ mono OLED | 128×64 | **I²C** | SSD1306 | **default** |
| 1.3″ mono OLED | 128×64 | I²C | SH1106 | firmware flag |
| 0.91″ mono OLED | 128×32 | I²C | SSD1306 | compact layout |
| 0.95″ **color** OLED | 96×64 | **SPI** | SSD1331 | *only if color required* — needs SPI pins (SCK/MOSI/DC/CS/RST), not I²C |

---

## 5. Inputs

- **Rotary encoder** (EC11-class): mechanical, ~20 detents/rev, quadrature A/B. Decoded in
  hardware by the **PCNT** peripheral (glitch filter enabled), so no steps are missed even
  under load. A/B get RC debounce (10 nF + internal pull-ups) in addition to the PCNT
  glitch filter.
- **Encoder switch (SW)** and **push button:** momentary, active-low with internal
  pull-ups; debounced in firmware (`components/button`) with short/long-press detection.
- **Fan-speed button (optional):** a dedicated momentary button on
  `CONFIG_THERMO_PIN_BTN_FAN` (default GPIO14, active-low, internal pull-up). Each short
  press cycles the fan speed **Auto → Low → Med → High → Auto**; the new speed shows in the
  OLED status bar and mirrors to the Matter Fan Control cluster. Leave the pin at −1 to use
  only the settings menu / Matter for fan control.
- **BOOT/reset button:** held **≥ 10 s** it opens an on-screen chooser — **Pairing**
  (re-open the Matter commissioning window) or **Factory reset** — rather than resetting
  immediately.
- **Occupancy / PIR sensor (optional):** a digital motion output (e.g. HC-SR501, AM312, or
  a PIR module) on `CONFIG_THERMO_PIN_OCCUPANCY`. Active-high by default (idle low, high on
  motion); the firmware enables the opposite internal pull so an unconnected pin reads "no
  motion". Powered from 3.3 V (AM312) or 5 V (HC-SR501 — level-check the output). Leave the
  pin at −1 to use only the manual Home/Away toggle.

## 6. Outputs (HVAC)

- 4 channels: **W** (heat), **Y** (cool/compressor), **G** (fan), **O·B** (reversing).
- Each channel: MCU GPIO → driver → load. Options:
  - **Mechanical relay** (SPST, 24 VAC/2 A) via NPN/MOSFET + flyback diode, or a
    ULN2003-style array. Add an RC snubber across contacts for inductive 24 VAC loads.
  - **Solid-state relay / triac** (e.g. opto-triac + MOC302x) for silent, long-life
    switching of 24 VAC — **recommended**, with opto-isolation from logic.
- **Isolation:** keep the 24 VAC domain optically isolated from the 3.3 V logic domain.
- **Fail-safe:** drivers are active-high; at reset/brown-out GPIOs are high-Z and the loads
  are **off**. Firmware also forces off on fault (FR-12).

### Fan speed (Low / Med / High / Auto)

The fan speed is a first-class control (settings menu + Matter Fan Control cluster). How it
reaches the blower depends on the wiring, all firmware-selectable:

- **Single-speed (default):** just the `G` relay. Any non-Auto speed = fan on; `Auto` runs
  the fan only during a heat/cool call.
- **Multi-speed blower:** wire the optional `G_LOW` / `G_MED` / `G_HIGH` tap relays (Kconfig
  pins, default −1 = unused). The firmware energizes exactly one tap for the active level
  (one-hot) plus `G` as a general enable.
- **ECM / 0–10 V / PWM (future hook):** map the level to a duty cycle on a spare LEDC pin;
  the level→output mapping lives in `components/relays`.

During a heat/cool call the fan runs at the configured **call speed** (`THERMO_FAN_CALL_SPEED`,
default High). A fixed Low/Med/High selection circulates continuously even when idle.

## 7. Status LED

On-board addressable **WS2812** RGB (GPIO8, RMT-driven). Color/behavior encodes state:

| State | LED |
|---|---|
| Uncommissioned / pairing | slow blue pulse |
| Joining Thread | blue blink |
| Idle (connected, no call) | dim green |
| Calling for heat | solid orange/red |
| Calling for cool | solid cyan/blue |
| Sensor/other fault | red blink |
| Identify (Matter) | white blink |

## 8. Power

- **Bench:** USB-C 5 V → 3.3 V buck/LDO for the module. Simplest for development.
- **Field (24 VAC HVAC):** isolated 24 VAC → 5 V converter (or C-wire), then 3.3 V rail.
  Add bulk capacitance to ride out radio TX current peaks (Thread/BLE bursts). A "C wire"
  (common) is required for continuous power; power-stealing is out of scope for v1.
- Decoupling per ESP32-C6-WROOM-1 datasheet; keep the antenna keep-out clear.

## 9. PCB / enclosure notes

- Keep the module antenna at a board edge with the vendor keep-out; no copper/metal near it.
- Keep the SHT40 I²C traces short; route them away from switching nodes.
- Physically and electrically separate the 24 VAC section (creepage/clearance) from logic.
- Mount the SHT40 away from the MCU/relays' self-heating and with airflow to the room —
  ideally vented at the enclosure edge, or on a short remote pigtail off the shared I²C bus.

## 10. Bill of materials

Summary — full list in [`hardware/bom.csv`](../hardware/bom.csv):

| Qty | Item | Notes |
|---|---|---|
| 1 | ESP32-C6-WROOM-1(U) module, 8 MB | Thread + BLE |
| 1 | SSD1306 128×64 I²C OLED | 0x3C |
| 1 | EC11 rotary encoder w/ switch | A/B/SW |
| 1 | Momentary push button | mode/back |
| 1 | Momentary push button | fan-speed cycle (Auto/Low/Med/High) |
| 1 | Sensirion SHT40 (SHT40-AD1B) | room sensor: temp + humidity, I²C 0x44 |
| 2 | 4.7 kΩ resistor | I²C pull-ups |
| 4 | Relay or opto-triac + driver | W/Y/G/O·B |
| 4 | Flyback diode / snubber | per relay |
| 1 | WS2812 RGB LED | status |
| 1 | Power supply (USB-C and/or 24 VAC→5 V iso) | |
| — | Decoupling caps, connectors, terminal blocks | |
