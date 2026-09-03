# Matter Hub (ESP32-C6)

An ESP32-C6 Matter **controller** + Thread **Border Router** — the brain of this
project. It commissions Matter-over-Thread end devices (the [RGB light](../rgb-light),
the [button](../button), third-party sensors/plugs), runs as the Thread border
router/leader, and exposes everything through two parallel interfaces: an interactive
**UART console** and a machine-readable **JSON protocol** over native USB.

Distilled from the ALPSTUGA sensor project into a generalized hub — there is **no
product-specific decoding**; each device's endpoints/clusters/attributes are
discovered at runtime. Current line **v1.9-dev** (released baseline **v1.8.0**).

## What it does

- **Commissioning** — pair one device (`pair`) or a batch (`pairlist`); `blescan`
  finds devices in pairing mode; `remove` decommissions. BLE-isolated, reboot-based.
- **Inspector** — walk any device: `devices`, `tree`, `ep`, `cluster`, `read`,
  `write`, `invoke`, `scan`, plus `on`/`off`/`toggle` shortcuts.
- **Device-to-device binding** — bind a switch to a plug/light with **single /
  double / long press** triggers; hub-mediated relay for event-only switches
  (e.g. IKEA BILRESA) or native Matter binding for capable sources.
- **Value logic** — rules over sensor values (temp / humidity / CO₂ / PM2.5) with
  hysteresis, boolean AND/OR/XOR/NOT, one level of `( )` grouping, driving OnOff
  targets; `track` (auto-inverse) or `oneshot`.
- **Time scheduling** — up to 4 schedulers (weekly + holiday), a shared holiday
  **calendar** (DD/MM with `**` wildcards); usable as logic conditions.
- **Config backup / restore** — the whole config as JSON; restore re-pairs by
  identity; enables hub-to-hub cloning.

Up to **7 paired devices** (heap-bound; event-only switches are cheaper — see
FOUNDATION.md).

## Two interfaces

| Interface | Port (this build) | Format | For |
|---|---|---|---|
| **Console** | UART0 — `COM12` @ 115200 | free-text + human output (carries `ESP_LOG`) | interactive use, a terminal |
| **JSON protocol** | native USB-Serial-JTAG — `COM7` @ 115200 | NDJSON (one JSON object per line) | scripts, GUIs, the Gateway Portal |

Both run at once, serialized by an internal command mutex. Flashing is over `COM12`.
The [Gateway Portal](../gateway-portal) drives the JSON port to put the hub on the web.

## Build & flash

ESP-IDF v5.4.4, target **esp32c6**. Two ways:

- **PlatformIO:** `pio run -t upload` (env `esp32-c6-devkitc-1`, `upload_port` COM12).
- **idf.py:** `idf.py set-target esp32c6 && idf.py build flash monitor`.

`ESP_MATTER_PATH` must point at an esp-matter checkout (or vendor it under
`managed_components/`). **Build from a space-free path** — CHIP/ESP-IDF break on paths
containing spaces. The esp-matter component needs the local patches in `patches/`,
which must be re-applied after any `managed_components` refresh — see
[patches/README.md](patches/README.md).

## Docs

- **[USER_GUIDE.md](USER_GUIDE.md)** — full console + JSON command reference, workflows, limits (also `docs/user_guide.html`).
- **[FOUNDATION.md](FOUNDATION.md)** — architecture and the invariants ("cardinal rules") any change must respect.
- **[STATUS.md](STATUS.md)** — current state / handoff notes.
