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

- `main/app_main.cpp` — Matter node + Extended Color Light endpoint (Hue/Sat mode),
  commissioning-window events drive the pairing LED flash.
- `main/app_driver.cpp` — WS2812 LED rendering, button (single-click toggle + the
  hold-15 s reset gesture with its yellow confirm flash), and the pairing slow-flash
  timer.
- `sdkconfig.defaults.esp32c6` — C6 + OpenThread + BLE-commissioning.
