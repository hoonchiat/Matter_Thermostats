# Hardware Design

Covers the block diagram, **pin assignment**, thermistor analog front-end and its math,
input/output stages, power, and the BOM. Machine-readable copies live in
[`hardware/pinout.csv`](../hardware/pinout.csv) and [`hardware/bom.csv`](../hardware/bom.csv).

---

## 1. Block diagram

```
                       ┌─────────────────────────────────────┐
   USB-C 5V ───▶ 3V3   │                                     │
   24VAC ─▶ [iso buck]─▶ LDO/buck ─▶ 3V3 ─▶ ESP32-C6-WROOM-1  │
                       │                                     │
   10K NTC ─[divider+RC]────────────────▶ ADC1_CH1 (GPIO1)  │
   OLED SSD1306 ◀──── I2C0 (SDA GPIO6 / SCL GPIO7) ─────────│
   Encoder A/B ─────────────────────────▶ PCNT (GPIO10/11)  │
   Encoder SW ──────────────────────────▶ GPIO2             │
   Push button ─────────────────────────▶ GPIO3             │
   RGB status LED ◀──── RMT (GPIO8)                          │
   Relays W/Y/G/OB ◀── GPIO18/19/20/21 ─▶ [drivers] ─▶ 24VAC │
   BOOT/reset btn ──────────────────────▶ GPIO9             │
                       └─────────────────────────────────────┘
```

---

## 2. Pin assignment (ESP32-C6-WROOM-1)

The ESP32-C6 GPIO matrix is flexible; the assignment below avoids the **strapping pins**
(GPIO4, 5, 8*, 9*, 15), the **USB-Serial-JTAG** pins (GPIO12/13), the **UART0 console**
(GPIO16/17), and the internal SPI-flash pins (GPIO24–30). ADC1 channels are GPIO0–GPIO6.

| Signal | GPIO | On-chip peripheral | Direction | Notes |
|---|---|---|---|---|
| **NTC sense** | GPIO1 | ADC1_CH1 | AIN | Divider node; 12-bit, 12 dB atten |
| **I²C SDA** (OLED) | GPIO6 | I2C0 | I/O | 4.7 kΩ pull-up to 3V3 |
| **I²C SCL** (OLED) | GPIO7 | I2C0 | O | 4.7 kΩ pull-up to 3V3 |
| **Encoder A / CLK** | GPIO10 | PCNT ch0 | IN | Hardware quadrature decode |
| **Encoder B / DT** | GPIO11 | PCNT ch0 | IN | Hardware quadrature decode |
| **Encoder switch** | GPIO2 | GPIO (ISR) | IN | Internal pull-up; press = select |
| **Push button** | GPIO3 | GPIO (ISR) | IN | Internal pull-up; mode / back |
| **Relay W** (heat) | GPIO18 | GPIO | OUT | Active-high to driver |
| **Relay Y** (cool/compressor) | GPIO19 | GPIO | OUT | Active-high to driver |
| **Relay G** (fan) | GPIO20 | GPIO | OUT | Active-high to driver |
| **Relay O·B** (reversing valve) | GPIO21 | GPIO | OUT | Heat-pump only |
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

## 3. Thermistor front-end

### 3.1 Divider topology

```
        3V3
         │
       [ R_fix = 10.0 kΩ, 0.1% ]        ← fixed reference resistor (top)
         │
         ├───────────┬──────────▶ GPIO1 / ADC1_CH1
         │         [ C = 100 nF ]        ← RC low-pass with R_series
       [ R_ntc ]      │                    (also add ~1–2 kΩ series R for ADC/ESD)
     10K NTC (Type2/3)│
         │            │
        GND          GND
```

With the NTC on the **bottom** leg, node voltage falls as temperature rises:

```
V_node = 3V3 · R_ntc / (R_fix + R_ntc)
```

Solving for the thermistor resistance from the measured node voltage:

```
R_ntc = R_fix · V_node / (Vref − V_node)
```

where `Vref` is the divider top rail (nominally 3.30 V). Because the ESP32 ADC is **not**
ratiometric to the supply (it references an internal ~1.1 V bandgap with attenuation), the
firmware uses the ESP-IDF **ADC calibration** API to convert raw counts → millivolts, and
`Vref` is a calibratable constant (`CONFIG_THERMO_DIVIDER_VREF_MV`). Powering the divider
top from a clean, known 3.3 V (or a dedicated reference) directly improves accuracy.

> **Design tip:** placing `R_fix` on top and the NTC on the bottom keeps the sense node at
> a comfortable mid-scale voltage around room temperature and lands the steepest part of
> the transfer curve in the comfort band, maximizing resolution where it matters.

### 3.2 Resistance → temperature

Two methods, both in `components/thermistor`:

1. **Lookup table (primary, recommended for HVAC accuracy).** A monotonic R→T table per
   curve (Type 2, Type 3) with linear interpolation between points. Generate it from the
   manufacturer R-T table (or from β) with [`tools/gen_ntc_lut.py`](../tools/gen_ntc_lut.py):

   ```bash
   python3 tools/gen_ntc_lut.py --type 3 --tmin -20 --tmax 60 --step 5 > firmware/components/thermistor/ntc_type3_lut.inc
   ```

2. **Steinhart–Hart / β model (fallback & interpolation).**

   β-model:
   ```
   1/T = 1/T0 + (1/β)·ln(R_ntc / R0)      T0 = 298.15 K, R0 = 10 kΩ
   ```
   Steinhart–Hart (more accurate over wide range):
   ```
   1/T = A + B·ln(R) + C·(ln R)³
   ```
   Coefficients are configurable per curve. Nominal starting values (validate against your
   sensor's datasheet):

   | Curve | β₍25/85₎ (K) | Notes |
   |---|---|---|
   | 10 kΩ Type 2 | ≈ 3891 | legacy/Honeywell-style "10K-2" |
   | 10 kΩ Type 3 | ≈ 3976 | common "10K-3" (BAPI/ACI-style) |

   The β values above are *nominal*; the LUT from the datasheet is authoritative.

### 3.3 Filtering & fault handling

- **Median-of-N** raw samples (default N=5) rejects impulse noise.
- **EMA** (exponential moving average, α configurable) smooths the temperature output.
- **Fault:** node voltage within a small band of 0 V or `Vref` ⇒ shorted or open sensor ⇒
  report fault, blank `LocalTemperature` behavior per Matter, and force all relays off.

### 3.4 Accuracy budget (typical, after 1-point calibration)

| Source | Contribution |
|---|---|
| NTC tolerance (±1 %) | ~±0.25 °C near 25 °C |
| R_fix 0.1 % | ~±0.03 °C |
| ADC calibration | ~±0.1 °C |
| Curve/LUT interpolation | ~±0.05 °C |
| **Net (comfort band)** | **≈ ±0.3 °C** (NFR-1) |

Self-heating is negligible: with `R_fix` = 10 kΩ the NTC dissipates ≲ 0.3 mW.

---

## 4. OLED display

- **Default:** SSD1306, 128×64, monochrome, I²C, address **0x3C** (0x3D selectable).
- **Alternates:** SH1106 128×64 (firmware flag), SSD1306 128×32 0.91″ (reduced layout).
- Shared I²C0 bus @ 400 kHz. Bus has headroom for a future humidity sensor (e.g. SHT4x).

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
- **BOOT/reset button:** doubles as the factory-reset input (long-press).

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
- Route the NTC sense pair away from switching nodes; guard the ADC input; single-point
  analog ground for the divider.
- Physically and electrically separate the 24 VAC section (creepage/clearance) from logic.
- Mount the NTC away from the MCU/relays' self-heating; ideally a remote/edge-vented probe.

## 10. Bill of materials

Summary — full list in [`hardware/bom.csv`](../hardware/bom.csv):

| Qty | Item | Notes |
|---|---|---|
| 1 | ESP32-C6-WROOM-1(U) module, 8 MB | Thread + BLE |
| 1 | SSD1306 128×64 I²C OLED | 0x3C |
| 1 | EC11 rotary encoder w/ switch | A/B/SW |
| 1 | Momentary push button | mode/back |
| 1 | 10 kΩ NTC, Type 2 or Type 3 | room sensor |
| 1 | 10.0 kΩ 0.1 % resistor | divider reference |
| 2 | 4.7 kΩ resistor | I²C pull-ups |
| 1 | 100 nF + 1–2 kΩ | ADC RC + series |
| 4 | Relay or opto-triac + driver | W/Y/G/O·B |
| 4 | Flyback diode / snubber | per relay |
| 1 | WS2812 RGB LED | status |
| 1 | Power supply (USB-C and/or 24 VAC→5 V iso) | |
| — | Decoupling caps, connectors, terminal blocks | |
