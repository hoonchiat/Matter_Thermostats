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

Modern layout inspired by the Honeywell Home app: a top **status bar**, a large centered
temperature, and a rounded **setpoint pill**.

```
┌────────────────────────────────────────────┐
│ ▲ HEAT                        HI  ✺  ●      │   status bar: mode + arrow | fan speed + fan icon | link dot
│────────────────────────────────────────────│
│                                            │
│            2 1 . 4 °C                       │   big current temperature (×3 font)
│                                            │
│   ╭──────────────────────────────────╮     │
│   │ SET 20.0°C                    ●  │     │   rounded setpoint pill (● = calling)
│   ╰──────────────────────────────────╯     │
│ HEATING                                    │   running state
└────────────────────────────────────────────┘
```

Status-bar elements (left → right):
- **Mode** with a filled ▲ (heat) / ▼ (cool) indicator; `AUTO` / `OFF` / `FAN` as text.
- **Fan speed** abbreviation `AU / LO / MD / HI` and an **animated fan icon** (spins while
  the fan is running).
- **Link dot**: filled when commissioned onto a Matter fabric, hollow ring when not.

Center / pill:
- **Current temperature** (large) from `LocalTemperature`, in the selected units with a
  degree mark.
- **Setpoint pill**: the active target (`SET …`); in **Auto** the pill shows both, each with
  its ▲/▼ marker. A filled dot appears in the pill while heating or cooling is called.
- **Running state** (bottom): `IDLE` / `HEATING` / `COOLING` / `FAN ON`, matching the LED.

These screens can be previewed as ASCII on a host PC — see [§9](#9-previewing-the-ui-no-hardware).

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

Long-press the push button (≥ 3 s) to open a scrollable list. **Rotate** the encoder to
move the highlight, **press the encoder** to activate the highlighted row, and **short-press
the push button** to go back / close.

### Implemented (v1)

| Row | Action | Persisted key |
|---|---|---|
| `FAN: AUTO/LOW/MED/HIGH` | encoder-press cycles the fan speed; mirrored to the Matter Fan Control `FanMode` attribute | `fan` |
| `PRESENCE: HOME/AWAY` | encoder-press toggles Home/Away (manual); Away switches to the unoccupied setpoints. In `SENSOR` mode this sets the manual preference and the sensor resumes on the next motion | `occHome` |
| `OCC SRC: MANUAL/SENSOR` | encoder-press chooses whether presence comes from the manual toggle or the PIR/occupancy sensor | `occSrc` |
| `UNITS: C/F` | encoder-press toggles °C ⇄ °F (also mirrors to the Matter `TemperatureDisplayMode` attribute) | `units` |
| `SENSOR: TYPE 2/3` | encoder-press toggles the NTC curve; re-applied to the sensor driver live | `ntcType` |
| `MATTER CODE >` | encoder-press opens the **INFO screen** showing the manual pairing code (the Matter setup payload number) | — |
| `BACK` | return to Home | — |

The list **scrolls** (5 rows visible) with a scrollbar when there are more items than fit;
the selection stays centered. When **Away**, an `AWAY` badge shows on the Home screen's
bottom row and the **unoccupied setpoints** are in effect (Matter OCC feature) — the Home
pill and the ADJUST screen then show/edit those (ADJUST reads `AWAY HEAT` / `AWAY COOL`),
so turning the encoder while Away changes the Away temperature, not the comfort one.

Fan speed can also be changed remotely from any Matter controller (see
[MATTER.md](MATTER.md)); local and remote stay in sync. `AUTO` runs the fan only during a
heat/cool call; `LOW/MED/HIGH` run it continuously at that speed (circulate).

The selected row is drawn as an inverted (highlighted) bar. Changes are persisted to NVS
immediately and, where relevant, pushed to Matter controllers.

### Planned (hooks already in the config/NVS)

Deadband, temperature calibration offset, min compressor off / min on, heat-pump reversing
mode, display brightness, and read-only Network/About views. These already exist as
`app_config_t` fields and `Kconfig` defaults; they only need menu rows added to the list in
`app_control.cpp` (`MENU_*` enum + `build_model()` line + a `menu_activate()` case).

## 5a. INFO screen (Matter code)

Reached from **Settings → `MATTER CODE >`**. Shows the Matter **manual pairing code**
(setup payload number) in large digits, so the device can be (re)commissioned without the
QR code. Encoder-press or push-button returns to the menu. The same code is generated from
the commissionable-data provider whether or not the device is currently commissioned.

```
┌────────────────────────────────────────────┐
│ MATTER CODE                                 │
│────────────────────────────────────────────│
│  1234-567-8901       ← large pairing code   │
│                                            │
│  SCAN QR OR ENTER                           │
└────────────────────────────────────────────┘
```

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

- A built-in **5×7 bitmap font** (`components/ui_oled/font5x7.inc`, generated and verified by
  [`tools/gen_font.py`](../tools/gen_font.py)) renders all text; the driver scales it (×3 for
  the big temperature). Lowercase is up-cased automatically, so the UI uses uppercase labels.
- Redraw runs at a ~10 Hz cap from `ui_task`; the whole 1 bpp framebuffer is flushed per
  frame (small enough over 400 kHz I²C).
- Icons (heat/cool triangles, the animated fan, the link dot, rounded pills) are drawn
  procedurally on the framebuffer — no image assets.
- The `ui_oled` component exposes a controller-agnostic surface (SSD1306 default, SH1106
  selectable) so the layout code is display-independent. It can optionally be backed by
  LVGL if a richer UI is desired later.

## 9. Previewing the UI (no hardware)

The rendering path (framebuffer + font + screen composition) compiles on a host PC when
`UI_OLED_HOST` is defined, so the exact pixels can be inspected without a display:

```bash
cd firmware/test/host
make preview      # renders HOME / ADJUST / MENU / INFO / PAIRING / FAULT as ASCII
```

This is how the layouts above are verified; only the I²C / esp_lcd bring-up and flush are
compiled out in that mode.
