# Matter Home Automation

ESP32 Matter projects (ESP-IDF v5.4.4 + the managed `espressif/esp_matter`
component). Each folder is a standalone ESP-IDF project.

| Folder | Board | What it is |
|---|---|---|
| `hub/` | ESP32-C6 | Matter controller + Thread Border Router + SRP server; schedule/logic engine, device bindings (single/double/long press), USB-JSON control protocol. |
| `gateway-portal/` | ESP32-S3 | Web control panel (Wi-Fi + WebSocket) bridged to the hub over USB-host CDC. |
| `rgb-light/` | ESP32-C6 | Matter Extended Color Light accessory (WS2812) - on/off, brightness, RGB; hold-BOOT-15 s factory reset. |
| `button/` | ESP32-C6 | Matter Generic Switch accessory - single / double / long press, RGB feedback; hold-BOOT-15 s factory reset. |
| `flash-tool/` | — | Self-contained browser (Web Serial) flasher with all four firmwares embedded; no toolchain needed. |

The two accessories share a **hold-BOOT-15 s reset-to-pair gesture**: hold the BOOT
button - nothing for 10 s, then the LED flashes yellow at a ~1 s interval for 5 s;
at 15 s the device factory-resets and reboots into pairing. Releasing before 15 s
cancels.

## Flashing without the toolchain

`flash-tool/flash-tool.html` is a self-contained page (open in desktop Chrome or
Edge) that flashes any of the four firmwares over USB, straight from the browser -
see [flash-tool/README.md](flash-tool/README.md).

## Building



`build/`, `managed_components/` and `sdkconfig` are gitignored. On a fresh
checkout of a subproject:

    idf.py set-target esp32c6      # esp32s3 for gateway-portal
    idf.py build

(`ESP_MATTER_PATH` must point at an esp-matter checkout for the C6 accessory
projects.)
