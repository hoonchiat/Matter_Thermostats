# Matter Data Model & Commissioning

Device type: **Thermostat (`0x0301`)**. Transport: **Thread**. Commissioning: **BLE**.
This document is the contract between the firmware and any Matter controller.

---

## 1. Endpoint / cluster map

### Endpoint 0 — Root Node (`0x0016`)

Provided by the stack. Key clusters:

| Cluster | ID | Role |
|---|---|---|
| Basic Information | 0x0028 | VID/PID, names, versions |
| General Commissioning | 0x0030 | commissioning flow |
| Network Commissioning | 0x0031 | **Thread** dataset provisioning |
| General Diagnostics | 0x0033 | diagnostics |
| Thread Network Diagnostics | 0x0035 | Thread health |
| OTA Software Update Requestor | 0x002A | firmware updates |
| Administrator Commissioning | 0x003C | open commissioning window |

### Endpoint 1 — Thermostat (`0x0301`)

| Cluster | ID | Role |
|---|---|---|
| Identify | 0x0003 | locate device (blink LED) |
| **Thermostat** | 0x0201 | core function |
| Thermostat User Interface Configuration | 0x0204 | display units, keypad lock |
| Groups | 0x0004 | *(optional)* |

---

## 2. Thermostat cluster (0x0201)

### Features declared

| Feature | Bit | Enabled | Meaning |
|---|---|---|---|
| Heating (`HEAT`) | 0 | ✅ | heating setpoint & W output |
| Cooling (`COOL`) | 1 | ✅ | cooling setpoint & Y output |
| Occupancy (`OCC`) | 2 | ❌ v1 | occupied/unoccupied sets |
| Schedule (`SCH`) | 3 | ❌ v1 | on-device schedule |
| Setback (`SB`) | 4 | ❌ v1 | setback |
| AutoMode (`AUTO`) | 5 | ✅ | System Mode = Auto w/ dead-zone |

### Attributes (implemented)

All temperatures are **signed int16 in 0.01 °C** (e.g. 2150 = 21.50 °C).

| Attribute | ID | Type | Access | Notes |
|---|---|---|---|---|
| LocalTemperature | 0x0000 | int16 | R | measured room temp; `null` on fault |
| AbsMinHeatSetpointLimit | 0x0003 | int16 | R | e.g. 700 (7 °C) |
| AbsMaxHeatSetpointLimit | 0x0004 | int16 | R | e.g. 3000 (30 °C) |
| AbsMinCoolSetpointLimit | 0x0005 | int16 | R | e.g. 1600 (16 °C) |
| AbsMaxCoolSetpointLimit | 0x0006 | int16 | R | e.g. 3200 (32 °C) |
| OccupiedCoolingSetpoint | 0x0011 | int16 | RW | default 2600 (26 °C) |
| OccupiedHeatingSetpoint | 0x0012 | int16 | RW | default 2000 (20 °C) |
| MinHeatSetpointLimit | 0x0015 | int16 | RW | user-limited range |
| MaxHeatSetpointLimit | 0x0016 | int16 | RW | |
| MinCoolSetpointLimit | 0x0017 | int16 | RW | |
| MaxCoolSetpointLimit | 0x0018 | int16 | RW | |
| MinSetpointDeadBand | 0x0019 | int8 | R(W) | Auto-mode min gap (0.1 °C units) |
| ControlSequenceOfOperation | 0x001B | enum8 | RW | 0x04 = Cooling & Heating |
| SystemMode | 0x001C | enum8 | RW | see table below |
| ThermostatRunningState | 0x0029 | map16 | R | bit0 Heat, bit1 Cool, bit2 Fan |

#### SystemMode (0x001C) values

| Value | Mode | This device |
|---|---|---|
| 0 | Off | ✅ |
| 1 | Auto | ✅ (requires AUTO feature) |
| 3 | Cool | ✅ |
| 4 | Heat | ✅ |
| 5 | Emergency Heat | mapped to Heat (aux) — v1: treat as Heat |
| 7 | Fan-only | ✅ (G only) |

#### ControlSequenceOfOperation (0x001B)

Set to **0x04 (Cooling and Heating)** since both W and Y are present. (0x02 = cooling
only, 0x00 = heating only — used automatically when a config disables one output.)

### Commands

| Command | ID | Handling |
|---|---|---|
| SetpointRaiseLower | 0x00 | adjust heat/cool/both by a delta; clamped to limits |

Local encoder adjustments produce the **same** setpoint writes internally, then call
`attribute::update()` so remote controllers stay in sync (bidirectional).

---

## 3. Thermostat User Interface Configuration (0x0204)

| Attribute | ID | Type | Notes |
|---|---|---|---|
| TemperatureDisplayMode | 0x0000 | enum8 | 0 = Celsius, 1 = Fahrenheit (display only; math stays °C) |
| KeypadLockout | 0x0001 | enum8 | 0 = none … lock local input |
| ScheduleProgrammingVisibility | 0x0002 | enum8 | *(v1: not used)* |

`TemperatureDisplayMode` is mirrored to the OLED units and persisted in NVS.

---

## 4. Basic Information (set these before shipping)

| Field | Value (example) |
|---|---|
| VendorName | *your name/org* |
| VendorID | `0xFFF1` (**test VID** — replace with a CSA-allocated VID for production) |
| ProductName | Matter Thread Thermostat |
| ProductID | `0x8001` |
| HardwareVersion / SoftwareVersion | per build |
| SerialNumber | per unit |

> **Production note:** shipping devices must use a CSA-allocated Vendor ID and a
> factory-provisioned **DAC/PAI** (Device Attestation Certificate chain) and a unique
> **Passcode/Discriminator** in the factory partition. Development builds use Espressif's
> test/attestation credentials and the default test setup code.

---

## 5. Commissioning flow (BLE → Thread)

**Prerequisite:** a **Thread Border Router** on the LAN — e.g. Apple TV 4K / HomePod mini,
Google Nest Hub (2nd gen) / Nest Wifi, Amazon eero, or an OpenThread Border Router (e.g.
Home Assistant + a Thread radio / SkyConnect).

```
1. Power on (uncommissioned) → device BLE-advertises; OLED shows QR + manual code.
2. Controller (Apple/Google/Alexa/SmartThings/HA) scans the QR / enters the code.
3. Controller connects over BLE, attests the device (DAC), then commissions:
     - creates a fabric, installs operational credentials (NOC),
     - pushes the Thread operational dataset (Network Commissioning cluster).
4. Device joins the Thread mesh, registers via SRP/mDNS, drops BLE.
5. Operational: controller talks to the Thermostat cluster over Thread.
```

Setup payload (dev defaults): Discriminator `0xF00`, Passcode `20202021`
(**development only** — never ship these).

### Multi-admin

Additional ecosystems pair via **Administrator Commissioning** (open a commissioning
window from an existing admin, then commission from the second controller) — the same
device joins multiple fabrics simultaneously.

---

## 6. Local ↔ Matter synchronization rules

| Trigger | Action |
|---|---|
| Remote write `SystemMode` | update `app_state.mode`, re-run control, redraw OLED |
| Remote write `OccupiedHeatingSetpoint` / `OccupiedCoolingSetpoint` | clamp to limits, update state, control, OLED |
| Remote write `TemperatureDisplayMode` | switch OLED units, persist |
| Local encoder setpoint change | update state → `attribute::update()` → controllers notified |
| Local mode change (button) | update state → `attribute::update(SystemMode)` |
| Measured temp change ≥ 0.1 °C or every N s | `attribute::update(LocalTemperature)` |
| Output state change | `attribute::update(ThermostatRunningState)` |
| Sensor fault | `LocalTemperature = null`, outputs off, running state cleared |

---

## 7. Factory reset / decommission

Local long-press (BOOT ≥ 5 s, confirmed on OLED) removes all fabrics and Thread
credentials and returns to step 1 above. Controllers should also be told to "remove" the
device to clean up their side.
