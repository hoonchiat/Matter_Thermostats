# Local UI — OLED Screens & Interaction Model

Display: SSD1306 128×64 monochrome. Inputs: rotary **encoder** (rotate + press) and a
**push button**. The UI is a small state machine; every input gives feedback in < 100 ms
(NFR-3).

---

## 1. Input model

| Input | Gesture | Home screen | In a menu |
|---|---|---|---|
| Encoder rotate | CW / CCW | adjust active setpoint ±0.5° | move selection / change value |
| Encoder press (SW) | short | toggle which setpoint is active (Heat/Cool) in Auto; else enter menu on the field | select / confirm |
| Push button | short | cycle System Mode: Off → Heat → Cool → Auto | back / cancel |
| Push button | long (≥3 s) | open Settings menu | exit to Home |
| BOOT button | long (≥5 s) | factory-reset confirmation | — |

An **adjust timeout** (default 4 s) commits a setpoint change and returns the home screen
to its resting layout. Changes are also pushed to Matter immediately.

---

## 2. Screen state machine

```
        ┌─────────┐  temp/mode/net updates
        │  HOME   │◀───────────────────────────┐
        └────┬────┘                             │
   rotate →  │ press(field)          button-long│
        ┌────▼────┐                        ┌────┴────┐
        │ ADJUST  │── timeout/press ──▶HOME │  MENU   │
        └─────────┘                         └────┬────┘
                                        press│    │button
   uncommissioned → PAIRING              ┌────▼──┐ │
   sensor fault    → FAULT               │ ITEM  │─┘ (edit value, press=save)
                                         └───────┘
```

- **PAIRING** is shown automatically while uncommissioned (overrides Home).
- **FAULT** is shown automatically on sensor fault (overrides Home; outputs are off).

---

## 3. HOME screen layout (128×64)

```
┌────────────────────────────────────────────┐
│ HEAT            �static status row      ⌂ ᯤ │   mode text · fabric · Thread signal
│                                            │
│      21.4°C        ← big current temp      │   large font, room temperature
│                                            │
│   Set 20.0°   ▲heat                        │   active setpoint + call indicator
│────────────────────────────────────────────│
│ ● heating          14:37   (optional)      │   running state · optional clock
└────────────────────────────────────────────┘
```

Elements:
- **Mode** (top-left): OFF / HEAT / COOL / AUTO / FAN.
- **Network** (top-right): commissioned/fabric icon + Thread link/signal glyph.
- **Current temperature** (center, large): from `LocalTemperature`, in the selected units.
- **Setpoint** row: the active target; in Auto shows both, highlighting the selected one.
- **Running state** (bottom): idle / heating / cooling / fan, matching the status LED.

## 4. ADJUST screen

Rotating on Home enters ADJUST: the setpoint enlarges and a ▲/▼ shows the change; each
detent = ±0.5° (clamped to min/max limits). Commit on press or after the timeout.

```
┌────────────────────────────────────────────┐
│  Set heating                                │
│                                            │
│        ►  20.5°C  ◄                         │
│                                            │
│   min 7.0    ▓▓▓▓▓▓░░░░░   max 30.0        │   position bar within limits
└────────────────────────────────────────────┘
```

## 5. SETTINGS menu

Long-press button opens a scrollable list; rotate to move, press to edit, button to go
back. Items:

| Item | Values | Persisted key |
|---|---|---|
| Units | °C / °F | `units` |
| Deadband | 0.2 – 3.0 °C | `deadband` |
| Temp calibration | −5.0 … +5.0 °C | `tempOffset` |
| Thermistor type | Type 2 / Type 3 | `ntcType` |
| Min compressor off | 60 – 900 s | `minOff` |
| Min on time | 30 – 600 s | `minOn` |
| Heat-pump reversing | Off / O in cool / B in heat | `hpReversing` |
| Display brightness | 0 – 255 | `brightness` |
| Network info | (view) IP, Thread role, RSSI | — |
| About | (view) FW ver, VID/PID | — |
| Factory reset | (confirm) | — |

## 6. PAIRING screen (uncommissioned)

```
┌────────────────────────────────────────────┐
│  Pair this thermostat                       │
│   ┌──────────┐                              │
│   │  ▓▓  ▓▓  │   ← Matter QR (setup payload)│
│   │  ▓ ▓▓▓ ▓ │                              │
│   └──────────┘   Code: 1234-567-8901        │   manual pairing code
│  Needs a Thread Border Router               │
└────────────────────────────────────────────┘
```

The QR encodes the Matter setup payload; the manual code is shown for controllers that
prefer typed entry.

## 7. FAULT screen (sensor fault)

```
┌────────────────────────────────────────────┐
│   ⚠  SENSOR FAULT                           │
│   Check room temperature sensor             │
│   (open / short detected)                   │
│   Outputs disabled                          │
└────────────────────────────────────────────┘
```

All HVAC outputs are forced off while a fault is active (FR-12); the status LED blinks red.

## 8. Rendering notes

- Redraw is event-driven with a ~10 Hz cap; only the dirty regions are pushed to reduce
  I²C traffic and flicker.
- Large-digit rendering uses a compact bitmap font; the small font handles labels.
- The `ui_oled` component exposes a controller-agnostic surface (SSD1306 default, SH1106
  selectable) so the layout code is display-independent. It can optionally be backed by
  LVGL if a richer UI is desired later.
