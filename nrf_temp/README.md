# Matter Temperature Sensor — nRF52840 SuperMini (Pro Micro)

A battery-powered **Matter over Thread** temperature sensor on a Nordic
**nRF52840 SuperMini / Pro Micro** module. It reads a **10K Type-3 NTC
thermistor** on the SAADC and reports it through the Matter
**TemperatureMeasurement** cluster, so it pairs to the Matter hub like the other
devices. It runs as a Thread **Sleepy End Device (SED)** for battery life, and a
button gives the same **hold-15 s reset-to-pair** gesture as the RGB light.

Flashing is by **UF2** (drag-and-drop over USB) — no debugger needed.

## What it is on the network

- **Device type:** Temperature Sensor (0x0302), endpoint 1.
- **Cluster:** TemperatureMeasurement (0x0402) `MeasuredValue`, updated every 30 s
  (hundredths of a degree Celsius).
- **Transport:** Thread, **Sleepy End Device** (Matter ICD). Vendor 0xFFF1 /
  product 0x8000.
- **Commissioning:** built-in test DAC. **Un-provisioned** units share the test
  setup code **20202021** / discriminator **3840** (0xF00); each unit can be given a
  **unique** code with the provisioning station (see *Per-device provisioning*).

## Wiring

The SuperMini has only an on-board LED (P0.15) and RESET. The **thermistor** and
the **pairing button** are external. Pins are set in
[`boards/promicro_nrf52840_nrf52840_uf2.overlay`](boards/promicro_nrf52840_nrf52840_uf2.overlay)
— change them there if your layout differs.

### Thermistor (10K Type-3 NTC), ratiometric divider

```
  P1.11 (THERM_PWR) ──[ 10K NTC ]──┬──[ 10K fixed ]── GND
                                   │
                                P0.04 (AIN2, divider midpoint)
```

| Signal | Pin | Note |
|---|---|---|
| Divider power | **P1.11** | firmware drives it HIGH only while sampling (battery life) |
| Divider midpoint | **P0.04 / AIN2** | SAADC input |
| Fixed resistor | 10 kΩ, 1% | node → GND |
| NTC | 10 kΩ Type-3 | THERM_PWR → node |

The read is **ratiometric** (ADC reference = VDD), so it is independent of the
battery voltage. If you flip the divider (NTC to GND) the temperature will read
inverted — keep NTC on the powered side as drawn.

### Pairing / reset button

| Signal | Pin | Note |
|---|---|---|
| Button | **P0.06** | momentary push-button **P0.06 ↔ GND** (active-low, internal pull-up) |

### LED

The on-board LED (**P0.15**) is the Matter status indicator: it flashes while a
commissioning window is open (pairing mode) and during the reset-confirm window.

## Behaviour

- **Pairing:** on first boot it opens a commissioning window automatically
  (LED slow-flash). Pair with setup code **20202021**.
- **Button — hold 15 s → factory reset & re-pair** (same as the RGB light):
  nothing for the first **10 s**, then the LED **blinks for 5 s** as a confirm
  window; at 15 s it wipes Matter commissioning and reboots into pairing.
  **Release any time before 15 s to cancel.**
- **Temperature:** sampled every 30 s and pushed to the hub's subscription.
- **Battery:** runs as a Thread SED. On the bench over USB the console/shell is
  live (USB keeps it awake); on battery it sleeps between check-ins.

## Flashing (UF2)

Your SuperMini must have the **Adafruit nRF52 UF2 bootloader** (most do). The app
is linked to `0x26000`, exactly where that bootloader loads it.

1. **Double-tap RESET** — the board mounts as a USB drive (e.g. `NRF52BOOT`).
2. Copy **[`nrf_temp-nrf52840-supermini.uf2`](nrf_temp-nrf52840-supermini.uf2)** onto
   that drive.
3. The board flashes and reboots into the app; a USB-CDC serial port appears
   (console + Matter shell).

> If the board has no UF2 bootloader yet, flash the Adafruit nRF52 bootloader once
> with a debugger (or `west flash` over J-Link), then use UF2 thereafter.

## Per-device provisioning (unique setup code)

Out of the box every unit shares the test code `20202021`. To give each unit a
**unique discriminator + passcode + serial** and a printable QR label, use the
browser [provisioning station](../provisioning-tool) — the same tool that
provisions the ESP32 Light/Button.

`src/provisioning.cpp` registers a custom `CommissionableDataProvider` (via Matter's
`mPreServerInitClbk`) that reads a 64-byte blob `{discriminator, passcode, serial}`
from the **`factory` partition (`0x0f3000`)** and computes the SPAKE2+ verifier
**on-device** — the same on-device-verifier scheme as the ESP32 devices, so the
browser writes only three small values (no crypto). A blank partition falls back to
the test defaults, so an un-provisioned board still boots and pairs.

The station hands you a tiny **identity `.uf2`** (16 blocks targeting `0x0f3000`,
independent of the app) to drag onto the board — so re-provisioning never touches the
app or its pairing. Flash the app UF2 once, then drop each unit's identity UF2.

## Build

Needs the NCS v2.9.3 toolchain set up in `C:\ncs` + `C:\nrf` (see the toolchain
notes). Then:

```bash
source /c/nrf/zenv.sh
cd /c/mhtemp
west build -b promicro_nrf52840/nrf52840/uf2 -d build . -- -DBOARD_ROOT=C:/mhtemp
# -> build/mhtemp/zephyr/zephyr.uf2
```

The `promicro_nrf52840` board is bundled out-of-tree under
[`boards/others/promicro_nrf52840`](boards/others/promicro_nrf52840) (LED P0.15,
UF2 flash layout, internal-RC 32 kHz clock).

## Notes / tuning

- **32 kHz clock:** defaults to the internal **RC** oscillator
  (`CONFIG_CLOCK_CONTROL_NRF_K32SRC_RC`) so it runs on SuperMini clones with no
  crystal. If your board has a 32.768 kHz crystal, switch to
  `CONFIG_CLOCK_CONTROL_NRF_K32SRC_XTAL=y` in `prj.conf` for better SED battery
  life.
- **Thermistor curve:** Beta model, R0 = 10 kΩ @ 25 °C, β = 3976 (Type-3). Tune
  the constants at the top of [`src/temperature_sensor.cpp`](src/temperature_sensor.cpp);
  for wide-range accuracy swap in an R-T lookup table.
- **Reset timing** (10 s + 5 s) lives in the shared NCS board module,
  `nrf/samples/matter/common/src/board/board_consts.h`
  (`kFactoryResetTriggerTimeout` / `kFactoryResetCancelWindowTimeout`).
- **No OTA / no MCUboot:** this is a single-image UF2 build. Firmware updates are
  re-flashed by UF2.

## Source

- `src/temperature_sensor.cpp` / `.h` — SAADC thermistor read (ratiometric,
  power-gated) + Type-3 conversion → TemperatureMeasurement.
- `src/app_task.cpp` — Matter node bring-up; registers the provisioning provider
  (pre-server-init) and starts the sensor after the server.
- `src/provisioning.cpp` / `.h` — per-device provisioning: reads {discriminator,
  passcode, serial} from the `factory` partition and computes the SPAKE2+ verifier
  on-device (browser tool writes it as a UF2). Blank → test creds.
- `src/default_zap/` — data model (root node + Temperature Sensor endpoint 1),
  generated from `template.zap`.
- `boards/others/promicro_nrf52840/` — out-of-tree board (UF2 layout with the 4K
  `factory` provisioning partition, USB-CDC console, LED P0.15).
- `boards/promicro_nrf52840_nrf52840_uf2.overlay` — thermistor ADC + power gate +
  external button.
- `prj.conf` / `Kconfig` / `sysbuild.conf` — Matter + Thread SED (MTD/ICD),
  test credentials, no-bootloader UF2.
