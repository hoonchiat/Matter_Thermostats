# Gateway Portal (ESP32-S3)

An ESP32-S3 (**N16R8**: 16 MB flash, 8 MB octal PSRAM) that puts the [Matter Hub](../hub)
on your network. It bridges the hub's USB JSON protocol to a browser: connect the S3
to the C6's native USB, join it to Wi-Fi, and control the whole Matter network from a
web page — no app, no cloud.

Firmware name `mh_s3_portal`, pure ESP-IDF v5.4.4.

## What it does

- **USB-host CDC bridge** to the C6 — speaks the hub's NDJSON JSON protocol
  (115200 8N1) over USB-host CDC-ACM.
- **Wi-Fi + web UI** — runs **AP+STA**: it joins your Wi-Fi (STA) and serves a
  single-page app + WebSocket at `http://<hostname>.local/`, while **at the same
  time** hosting a SoftAP (`MatterGateway-XXXX`, captive portal at `192.168.4.1`) so
  the config page is always reachable even before Wi-Fi is set up.
- **Live control** — device list & status, on/off, brightness & RGB for the light,
  bindings (incl. press type) and logic — all pushed live over the WebSocket.
- **Time sync** — NTP → hub `settime` → per-device `devtime`, so schedules and CASE
  cert validity have a real clock.
- **Configuration tab** — Wi-Fi / AP / hostname / NTP / timezone / poll interval,
  persisted in NVS and editable from the browser (`cfg_get` / `cfg_set` / `s3_reboot`).
  Timezone and poll apply live; network changes apply on reboot.

## Wi-Fi setup

Network credentials live in NVS and are set from the SPA's **Configuration** tab
(passwords are write-only — they are never returned to the browser). First-boot
defaults are compiled into `main/cfg.cpp`; either change them there, or just boot,
connect to the SoftAP, open `192.168.4.1`, and set Wi-Fi from the page.

## Build & flash

ESP-IDF v5.4.4, target **esp32s3** (the board must be **N16R8** — 16 MB flash, 8 MB
octal PSRAM):

```
idf.py set-target esp32s3
idf.py build flash monitor
```

The SPA in `main/www/index.html` is embedded into the firmware at build time; the
console is on the S3's UART.

## Layout

| File | Role |
|---|---|
| `main/usb_bridge.*` | USB-host CDC-ACM link to the C6 (NDJSON) |
| `main/net.*` | Wi-Fi AP+STA, mDNS, SNTP, captive-portal DNS |
| `main/web.*` | HTTP server + WebSocket, serves the SPA |
| `main/portal.*` | app task: status polling + time sync + web pushes |
| `main/cfg.*` | persisted portal config (NVS) |
| `main/www/index.html` | the browser control panel |
