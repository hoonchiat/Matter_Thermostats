# Matter RGB Light (ESP32-C6, Thread)

A Matter **Extended Color Light** accessory on an ESP32-C6-DevKitC-1. The onboard
WS2812 RGB LED (GPIO8) emulates a multi-color bulb — On/Off, brightness (Level
cluster) and RGB color (Color Control, Hue/Saturation). Commissions over BLE and
runs on **Thread**, so it joins the hub's Thread network and any Matter controller
(including the hub) can pair and drive it.

Forked from esp-matter's `examples/light` and rewired to build against the **managed
`espressif/esp_matter` component** (same as the hub) instead of a bootstrapped
connectedhomeip.

## Behavior

- **RGB + brightness** — controlled from any Matter app/controller.
- **BOOT button (GPIO9):**
  - **short press** → toggle the light on/off (local control, handy for testing).
  - **hold 15 s** → factory-reset & re-pair: nothing for 10 s, then the LED flashes
    **yellow** at a ~1 s interval for 5 s; at 15 s it wipes commissioning and reboots
    into pairing. Releasing any time before 15 s cancels and restores the bulb.
- **LED indicator:**
  - **slow blue flash (~0.7 s)** while a commissioning window is open = *pairing mode*.
  - shows the live bulb color/brightness once commissioned.
- Test commissioning credentials: setup code **20202021**, discriminator **3840**.

## Mesh (Thread Router)

The light is a mains-powered Full Thread Device and, on attaching to the network,
actively requests the **Router** role (`otThreadSetRouterEligible` +
`otThreadBecomeRouter` on attach). So it routes for other nodes and **extends the
Thread mesh** — range and redundancy — instead of sitting as a leaf end-device.

## Occupancy auto-off (optional PIR)

Wire a PIR / occupancy sensor to a GPIO and the light turns itself **off** after no
presence for a configurable period (default **10 min**). It is entirely local — it
does **not** depend on the hub; the hub only *sets the period*.

**Wiring — sensor to the ESP32-C6:**

| Sensor lead | ESP32-C6 pin |
|---|---|
| OUT — digital, **active-HIGH** on presence | **GPIO10** |
| VCC | 3V3 or 5V (per your PIR module) |
| GND | GND |

- The input uses an internal **pull-up**, so with **no sensor wired** the pin reads
  HIGH = "always present" → the light **never auto-offs** and behaves normally.
  (*This is how "sensor not present → works as normal" is achieved.*)
- Presence keeps the light on; after the period with no presence, `OnOff` is set off
  (LED off **and** reported to the hub). It does not auto-turn-*on*.

**Setting the period from the hub** (over Matter): write the custom attribute
`(light endpoint, cluster `0x0006` OnOff, attribute `0xFFF10000`, uint16 = minutes)` —
e.g. 5 / 10 / 15 / 30. Persisted on the light (NVS); default 10, clamped 1–1440.

## Build & flash

Needs ESP-IDF v5.4.4 and the esp-matter repo (for the `device_hal` LED/button drivers
+ `app_reset`). Matter core is pulled as a managed component.

```
# PowerShell
. C:\esp\v5.4.4\esp-idf\export.ps1
$env:ESP_MATTER_PATH = "C:/esp/esp-matter"      # forward slashes (CMake)
idf.py set-target esp32c6
idf.py -p COM13 build flash monitor
```

The C6 is on **COM13** (CH343 UART). First build is long (compiles the Matter SDK).

## Source

- `main/app_main.cpp` — Matter node + Extended Color Light endpoint (Hue/Sat mode);
  commissioning-window events drive the pairing LED flash; requests the Thread
  **Router** role; and adds the occupancy-timeout attribute (`0xFFF10000` on OnOff).
- `main/app_driver.cpp` — WS2812 LED rendering, button (single-click toggle + the
  hold-15 s reset gesture with its yellow confirm flash), and the pairing slow-flash
  timer.
- `main/occupancy.cpp` / `main/occupancy.h` — occupancy input (GPIO10, active-high +
  pull-up) and the local auto-off timer (turns `OnOff` off after the period; the
  period is hub-settable via the OnOff attribute and persisted in NVS).
- `sdkconfig.defaults.esp32c6` — C6 + OpenThread (FTD/router) + BLE-commissioning.
