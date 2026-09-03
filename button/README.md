# Matter Button (ESP32-C6, Thread)

A Matter **Generic Switch** accessory on an ESP32-C6-DevKitC-1 — a single momentary
button (the onboard **BOOT** button, GPIO9) that raises Matter Switch events over
**Thread**. It commissions over BLE, joins the hub's Thread network, and can be bound
(from the hub) to drive other devices — a plug, the RGB light, etc.

Forked from the RGB-light project and rewired to a Generic Switch; builds against the
managed `espressif/esp_matter` component (same as the hub), not a bootstrapped
connectedhomeip.

## Behavior

Three gestures on the BOOT button map to distinguishable Matter Switch (cluster
0x003B) events, each with a brief RGB flash for feedback (the LED is off otherwise):

| Gesture | Matter event | LED |
|---|---|---|
| **Single press** | `MultiPressComplete(count=1)` | green |
| **Double press** | `MultiPressComplete(count=2)` | blue |
| **Long press** (hold ≥ 1 s) | `InitialPress` + `LongPress` + `LongRelease`, **on release** | red |

- The **long press is deferred to release** so it composes cleanly with the reset
  gesture below — a reset hold emits no switch event.
- **Hold 15 s → factory-reset & re-pair:** nothing for 10 s, then the LED flashes
  **yellow** at a ~1 s interval for 5 s; at 15 s it wipes commissioning and reboots
  into pairing. Releasing before 15 s cancels. A hold that reaches the 10 s mark
  sends **no** switch event.
- **Pairing indicator:** the LED slow-flashes **blue** (~0.7 s) while a commissioning
  window is open.
- **Identity:** Vendor "Daikin PJoshua", Product "PJoshua Button", VID 0xFFF1 / PID
  0x8001, plus a SerialNumber — via `main/CHIPProjectConfig.h`,
  `CONFIG_DEVICE_PRODUCT_ID`, and `create_serial_number()` in `app_main.cpp`.
- Test commissioning credentials: setup code **20202021**, discriminator **3840**.

## Timing

- **Long-press threshold** — `CONFIG_BUTTON_LONG_PRESS_TIME_MS = 1000`.
- **Double-press window** — `CONFIG_BUTTON_SHORT_PRESS_TIME_MS = 600`: iot_button
  waits this long after a release for a second press, so a single press fires ~600 ms
  after release (the trade for reliable double-press detection).

## Build & flash

Needs ESP-IDF v5.4.4 and the esp-matter repo (for the `device_hal` LED/button
drivers). The Generic Switch cluster server needs `CONFIG_SUPPORT_SWITCH_CLUSTER=y`
(already in `sdkconfig.defaults.esp32c6`).

```
# PowerShell
. C:\esp\v5.4.4\esp-idf\export.ps1
$env:ESP_MATTER_PATH = "C:/esp/esp-matter"      # forward slashes (CMake)
idf.py set-target esp32c6
idf.py -p COM12 build flash monitor
```

The C6 is on **COM12** (CH343 UART). First build is long (compiles the Matter SDK).

## Source

- `main/app_main.cpp` — Matter node + Generic Switch endpoint (momentary switch
  features MS/MSR/MSL/MSM set in the config *before* `create()`), SerialNumber, and
  commissioning-window events driving the pairing LED flash.
- `main/app_driver.cpp` — BOOT button → Switch events (single/double, long-press
  deferred to release), per-gesture LED flash, the hold-15 s reset gesture, and the
  pairing slow-flash timer.
- `sdkconfig.defaults.esp32c6` — C6 + OpenThread + BLE-commissioning +
  `CONFIG_SUPPORT_SWITCH_CLUSTER=y` + PID 0x8001 + button timing.
