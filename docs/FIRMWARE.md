# Firmware Architecture

Target: **ESP-IDF v5.2+** and **ESP-Matter** (CHIP / connectedhomeip). Language: C for
drivers and control logic, C++ for the `app_main` / Matter glue (ESP-Matter is C++).

---

## 1. Component map

```
firmware/
├── main/
│   ├── app_main.cpp          # entry: init stack, create Matter endpoints, start tasks
│   ├── app_priv.h            # shared app state, event definitions, config struct
│   ├── app_matter.cpp/.h     # endpoint/cluster creation + attribute callbacks (glue)
│   ├── app_control.cpp/.h    # bridges sensor→control→relays and control→Matter
│   ├── app_nvs.cpp/.h        # load/save persisted config
│   └── Kconfig.projbuild     # all pins & defaults exposed to menuconfig
└── components/
    ├── thermistor/           # ADC + NTC(Type2/3) → °C          [implemented]
    ├── rotary_encoder/       # PCNT quadrature + switch          [implemented]
    ├── button/               # debounce + short/long press       [implemented]
    ├── thermostat_core/      # mode/hysteresis/cycle-timer law    [implemented, pure]
    ├── relays/               # 4-ch HVAC output driver           [implemented]
    └── ui_oled/              # SSD1306 screens + 5x7 font + menu   [implemented]
```

**Design principle:** the *decision-making* logic (`thermostat_core`) is a pure state
machine with **no I/O and no RTOS dependency** — it takes a snapshot of inputs and returns
the desired output state. That makes it trivially unit-testable on the host and keeps HVAC
safety logic isolated from Matter/driver churn.

---

## 2. Tasks, queues & shared state

```
   ┌────────────┐  temp (0.01°C)   ┌──────────────┐  desired outputs  ┌──────────┐
   │ sensor_task│ ───────────────▶ │ control_task │ ────────────────▶ │ relays   │
   │  (1 Hz)    │                  │  (2 Hz tick) │                   └──────────┘
   └────────────┘                  │              │  running state ─▶ Matter attr
                                    └──────▲───────┘
   ┌────────────┐  input events           │ setpoint/mode changes
   │  ui_task   │ ─── app_event queue ─────┘ ▲
   │ enc+btn+   │ ◀─────────────────────────┘ (state snapshot for display)
   │ OLED draw  │
   └────────────┘        ┌──────────────┐
                         │ Matter/CHIP  │  attribute writes (remote) → app_event queue
                         │  event loop  │  local changes → attribute::update()
                         └──────────────┘
```

- **`app_event` queue** — a single FreeRTOS queue carries typed events
  (`EVT_ENCODER_DELTA`, `EVT_BUTTON_SHORT/LONG`, `EVT_MATTER_SET_MODE`,
  `EVT_MATTER_SET_HEAT`, `EVT_MATTER_SET_COOL`, `EVT_SENSOR_FAULT`, …).
- **`app_state`** — the single source of truth (setpoints, mode, measured temp, running
  state, units, config). Guarded by a mutex; mutated only by `control_task` and the config
  loader, read by everyone.
- **Rate limiting** — `LocalTemperature` is reported to Matter on change ≥ 0.1 °C or every
  N seconds, whichever first, to avoid flooding the network.

### Task summary

| Task | Prio | Period | Responsibility |
|---|---|---|---|
| `sensor_task` | med | 1 Hz | sample ADC, convert, filter, publish temp + fault |
| `control_task` | high | 0.5 s | run `thermostat_core`, drive relays, timers, push Matter attrs |
| `ui_task` | low | event + 10 Hz redraw | encoder/button handling, OLED rendering |
| CHIP event loop | (stack) | — | Matter interaction model, Thread |

---

## 3. Control algorithm

Implemented in `components/thermostat_core`. Inputs: measured temp, mode, heat/cool
setpoints, deadband, timers/config, `now_ms`. Output: desired W/Y/G/O·B + running state.

### 3.1 Per-mode logic (hysteresis)

Let `db = deadband` (default 1.0 °C). Half-band `h = db/2`.

```
OFF:                 all outputs off.

HEAT / (AUTO heat):  if T <= heatSet - h   → call heat  (W on)
                     if T >= heatSet + h   → stop heat  (W off)
                     else hold previous.

COOL / (AUTO cool):  if T >= coolSet + h   → call cool  (Y on)
                     if T <= coolSet - h   → stop cool  (Y off)
                     else hold previous.

AUTO:                enforce coolSet - heatSet >= min_deadzone (default 2°C);
                     never call heat and cool simultaneously; heat wins ties only
                     below heatSet, cool only above coolSet; dead-zone in between = idle.

FAN_ONLY:            G on, W/Y off.
```

Fan speed (Auto/Low/Med/High) → an output `fan_level` (0..3):
```
fan_level = (heating || cooling) ? fan_call_speed : 0   // AUTO follows the call
if (fan_speed is LOW/MED/HIGH) fan_level = max(fan_level, fan_speed)  // continuous circulate
OFF mode: fan_level = fixed speed (0 if AUTO);  FAN_ONLY: fixed speed or call speed;  fault: 0
```
`G` = `fan_level > 0` (fan enable). Optional `G_LOW/G_MED/G_HIGH` taps are driven one-hot
from `fan_level` for a multi-speed blower.
Reversing valve (O·B): heat-pump config maps a cool call → O (or heat call → B).

### 3.2 Compressor / cycle protection (safety, NFR-4)

Applied *after* the hysteresis decision, as a gate:

```
min_off:  a compressor output (Y, and W if heat-pump) that just turned OFF
          cannot turn back ON until min_off_s elapsed (default 300 s).
min_on:   once ON, stay ON at least min_on_s (default 120 s) unless mode→OFF.
startup:  after boot/power-up, hold a startup_lockout_s (default 30 s) before any
          compressor call.
```

The gate can *delay* a call but never *forces one on*; on any fault or OFF mode all outputs
drop immediately (min-on does not override safety-off).

### 3.3 Pseudocode

```c
thermo_output_t thermo_core_step(const thermo_input_t *in, thermo_state_t *st) {
    thermo_output_t out = {0};              // default: everything OFF (fail-safe)
    if (in->fault || in->mode == MODE_OFF) { st->last_change_ms = in->now_ms; return out; }

    float h = in->deadband_c / 2.0f;
    bool want_heat = false, want_cool = false;

    switch (in->mode) {
      case MODE_HEAT: want_heat = hysteresis(in->temp_c, in->heat_set_c, h, st->heating); break;
      case MODE_COOL: want_cool = hysteresis_cool(in->temp_c, in->cool_set_c, h, st->cooling); break;
      case MODE_AUTO:
        want_heat = hysteresis(in->temp_c, in->heat_set_c, h, st->heating);
        want_cool = hysteresis_cool(in->temp_c, in->cool_set_c, h, st->cooling);
        if (want_heat && want_cool) { want_heat = want_cool = false; } // safety
        break;
      case MODE_FAN_ONLY: out.g_fan = true; return out;
      default: break;
    }
    // apply cycle-protection gates (min_on/min_off/startup) → commit → set out.*
    ...
    out.g_fan = out.w_heat || out.y_cool || in->fan_request;
    out.ob_reversing = heat_pump_valve(in, out);
    return out;
}
```

The reference implementation with the full gate is in
`components/thermostat_core/thermostat_core.c`.

---

## 4. UI / input handling

- **Encoder** → PCNT count deltas → `EVT_ENCODER_DELTA` (±steps). On the home screen a
  delta nudges the *active* setpoint by the step (0.5 °C / 0.5 °F); in a menu it moves the
  selection.
- **Encoder switch (select)** → enter/confirm menu items.
- **Push button** → short: cycle mode (Off→Heat→Cool→Auto) or "back" in a menu; long
  (≥ 3 s on home): open settings menu.
- **BOOT/reset** → long (≥ 5 s): factory-reset confirmation.
- **Debounce:** `components/button` debounces (default 30 ms) and emits SHORT on release
  and LONG at the threshold.

The screen model is a small state machine (Home / Adjust / Menu / Confirm / Pairing /
Fault). See [UI.md](UI.md).

---

## 5. Matter integration (glue)

`app_matter.cpp` creates the endpoints/clusters and registers callbacks:

- **Endpoints:** Root (0), Thermostat 0x0301 (1), and Fan 0x002B (2, Fan Control cluster).
- **Attribute update callback** (remote write, `PRE_UPDATE`): translate `SystemMode`,
  `OccupiedHeatingSetpoint`, `OccupiedCoolingSetpoint`, `TemperatureDisplayMode`, and
  `FanControl::FanMode`/`PercentSetting` writes → `app_event`s → update `app_state` →
  re-run control. This is the **remote override** path for mode, setpoints and fan speed.
- **Local → Matter:** when the encoder/button/menu change a setpoint, mode, units, or fan
  speed, call `esp_matter::attribute::update()` so controllers see the change (bidirectional).
- **Pairing:** mirrors the esp-matter `light` example — `esp_matter::start(app_event_cb)`,
  `PrintOnboardingCodes(BLE)`, and re-open a DNS-SD commissioning window on last-fabric
  removal.
- **Identify:** blink the status LED.
- Temperatures cross the boundary in Matter's units: **0.01 °C signed int16** for
  `LocalTemperature` and the setpoints.

Because exact enum/attribute IDs and config structs track the installed ESP-Matter
version, those touch-points are marked with `// TODO(matter):` in the glue files and
referenced to [MATTER.md](MATTER.md).

---

## 6. Persistence (NVS)

`app_nvs` loads config at boot into `app_state` and writes back (debounced, ~2 s after the
last change) whenever the user changes a setting. Keys are listed in
[SPECIFICATION.md §11](SPECIFICATION.md#11-persistence). Matter fabric/Thread credentials
are stored by the stack in its own NVS partition.

---

## 7. Build, flash, config

```bash
cd firmware
idf.py set-target esp32c6
idf.py menuconfig      # → "Matter Thermostat" : pins, thermistor type, deadband, timers…
idf.py build
idf.py -p <PORT> flash monitor
```

Key `sdkconfig.defaults` selections: enable **Thread (OpenThread)**, **BLE** for
commissioning, **Matter**, set partition table to the provided `partitions.csv` (factory +
OTA A/B + NVS). See `firmware/sdkconfig.defaults` and `firmware/sdkconfig.defaults.esp32c6`.

---

## 8. Testing strategy

- **Host unit tests** for `thermistor` (R→T against known points) and `thermostat_core`
  (hysteresis boundaries, min-off/min-on gating, auto dead-zone, fan-speed levels,
  fail-safe). These components are pure C with no ESP dependency, so they compile and run on
  a PC: `cd firmware/test/host && make`.
- **Host UI preview:** `make preview` renders every OLED screen to the terminal as ASCII
  (the `ui_oled` drawing path compiles under `UI_OLED_HOST`), so layouts are verifiable
  without hardware.
- **On-target smoke:** verify ADC↔temp with a reference thermometer; encoder count
  stability; relay actuation with an LED load before wiring 24 VAC; commissioning against a
  real Border Router + controller.
- **Soak:** confirm min-off never violated under rapid setpoint changes; Thread rejoin
  after router reboot.
