# Matter Hub V1.0 — Foundation & Knowledge Base

A clean-slate distillation of everything learned building an **ESP32-C6 Matter
controller/hub** (from the ALPSTUGA project). This is the seed for Matter Hub
V1.0 — read it first. It is generalized: no product-specific sensor details,
just the architecture and the hard-won gotchas.

> **The single most important lesson:** the Matter/Thread commissioning path is
> fragile and radio-timing-sensitive. Once it works, treat it as sacred.
> **Add features additively; never restructure a working commissioning path.**
> A whole release was lost re-architecting commissioning to add a UI feature that
> never needed to touch it.

---

## 1. What this device is

An ESP32-C6 acting as a **Matter *controller* (commissioner)** — not a Matter
*accessory/server*. It:
- Commissions **Matter-over-Thread** end devices (over BLE, then they join Thread).
- Runs as the **Thread Border Router / Leader** (native 802.15.4 radio).
- **Subscribes** to the devices' attributes and presents/forwards the data.

It uses the ESP-Matter **controller** APIs (`esp_matter::controller::…`), *not*
`esp_matter::start()` (server mode). In controller mode the Thread stack is **not**
auto-initialised by the framework — you must `InitThreadStack()` +
`StartThreadTask()` yourself.

## 2. Hardware & toolchain

- **MCU:** ESP32-C6 (single-core RISC-V + LP core, 320 KB RAM, 4–8 MB flash).
- **Radios:** ONE 2.4 GHz radio shared by **BLE + IEEE 802.15.4 (Thread) + Wi-Fi**
  via coexistence. This sharing is the source of most pain (see §4).
- **Two serial ports:**
  - **UART bridge** (e.g. CH343) → the **console** (`ESP_LOG`, `printf`, command
    input). DTR/RTS wired to EN/GPIO9 — a *reset hazard*.
  - **Native USB-Serial/JTAG** (VID:PID `303A:1001`) → free for a second channel
    (used for the web dashboard). Opening it also resets the chip (DTR/RTS).
- **SDK:** ESP-IDF v5.4.4, esp-matter (connectedhomeip). Build via `idf.py` /
  PlatformIO (espidf framework). Watch Windows MAX_PATH with the CHIP SDK — redirect
  `IDF_COMPONENT_CACHE_PATH` to a short path (e.g. `C:\ecp`).

## 3. Architecture overview

```
   Browser (Web Serial)  ──USB-JTAG (COM7)──┐
                                             ▼
   Console/UART (COM12) ◄──────────────  ESP32-C6 Matter Hub
                                             │  controller + Thread BR/Leader
                                    BLE (commission) + 802.15.4 (Thread)
                                             ▼
                                   Matter-over-Thread end devices
```

- **Boot modes** (decided from NVS at boot, one radio config per boot):
  - **Operating mode** (≥1 device paired): BLE **off**, Thread on, subscribe to all
    paired devices. This is where monitoring happens.
  - **Commissioning mode** (0 devices, or a one-shot "add a device" flag): BLE **on**
    for commissioning, **no active subscriptions**. Radio is free for reliable BLE.
- **Mode switching = reboot.** `pair` sets a one-shot NVS flag and `esp_restart()`s
  into commissioning mode; on success it reboots back to operating. This reboot-based
  isolation is deliberate and **essential** (see §5).

## 4. The radio-coexistence reality (read this twice)

The C6 has one radio. Concurrent RF users **starve each other**:

- **BLE commissioning + active Thread traffic = failure.** If you commission over
  BLE while the Thread network is live (subscriptions running, hub routing), the
  802.15.4 MAC logs `ChannelAccessFailure` and the commissioning stalls — either
  the device is never discovered, or it joins Thread then can't complete CASE.
  → **Fix: commission in an *isolated* session with no subscriptions.** That's why
  pairing reboots into commissioning mode (nothing else on the radio).
- **Wi-Fi + Thread cannot coexist** on the C6 (BLE/Wi-Fi init aborts the other's
  PHY; RAM corruption seen). If you need Wi-Fi (e.g. NTP), **time-share**: run a
  boot-only Wi-Fi window, tear it fully down, *then* start Thread. Or avoid Wi-Fi
  entirely and get time from a connected host (see §8).
- BLE + Thread **can** coexist *briefly* for commissioning (that's normal Matter),
  but only reliably when Thread is otherwise quiet.

## 5. Commissioning flow and its fragile steps

`pairing_code_thread(node_id, payload, thread_dataset)` drives BLE→Thread
commissioning. Stages (all must pass):
1. **BLE discovery** — find the device advertising. Times out if the device isn't
   in pairing mode *at the moment the hub scans* (windows are short; arm the device
   right before, keep it close).
2. **PASE** (over BLE) — passcode-authenticated session. Fast (~seconds) when found.
3. **Attestation** — 3rd-party devices (e.g. IKEA) fail with `PAA not found in DCL`.
   **Override and continue** (accept the attestation failure) or they never pair.
4. **OpCert / NOC** — operational cert signing.
5. **Operational discovery + CASE** (`kFindOperationalForStayActive/…Complete`) —
   the device joins Thread, the hub resolves its operational address via **mDNS/SRP
   over Thread**, then establishes CASE. **This is the most fragile step.** It fails
   (`operational discovery failed: 32` timeout) if the device **detaches from Thread**
   before CASE, or mDNS/SRP resolution is slow/broken.
   - Mitigation: register a **Thread link-local peer hint** (fe80:: from the device's
     EUI-64) so CASE can reach it without waiting on mDNS multicast — important for
     sleepy end devices that don't receive `ff02::fb` until they data-poll.
6. On success: mark paired in NVS, subscribe immediately (before the device's
   post-commission StayActive window expires), then reboot to operating mode.

**Add a watchdog:** a failed attempt can hang with no callback and then **block the
next attempt** with `ESP_ERR_INVALID_STATE` (there is *no* stop/cancel API — only
`unpair_device`). Arm a timer on commission start; on timeout, report failure and
**reboot to operating** so the hub never gets stuck in commissioning mode.

## 6. Device lifecycle — pair / remove / decommission

- **Pair:** reboot → isolated commissioning → auto-fire commissioning (equivalent to
  a BOOT-button press) so it's hands-off → reboot back. Auto-commission is additive:
  it just *triggers* the same `do_commission()`, it does not change the mechanics.
- **Remove:** **must decommission**, not just drop the local record. Call
  `unpair_device(node_id)` (RemoveFabric) *and* shut down the subscription
  (`InteractionModelEngine::ShutdownSubscriptions(fabricIndex, nodeId)`). If you only
  clear NVS, the device **stays commissioned to your fabric, won't advertise for
  pairing**, and can't be re-paired; plus **orphaned devices linger on the Thread
  network** and destabilise it (new joiners detach). This bit us badly.
- Node ids: assign a fixed operational node id per slot (`BASE + slot`) so state is
  deterministic and re-pairs reuse the slot cleanly.

## 7. Heap discipline

The C6 is **heap-tight**, worst in BLE-commissioning mode (~15 KB free).
- `xTaskCreate` **silently fails (returns -1)** at low heap — the task just never
  runs (a "dead console" symptom). Always check the return.
- **Create essential tasks first**; defer/gate heavy ones (subscriptions, extra
  channels) behind mode and a free-heap check. Log `esp_get_free_heap_size()` after
  task creation.
- Stack sizing matters: functions that `snprintf` floats into large buffers can
  overflow a 2 KB stack (stack-protection fault). Budget ≥4 KB for such tasks.

## 8. Time

Matter uses real time (cert validity, CASE). The hub needs a time source:
- **From a connected host** (recommended if a UI is always attached): push
  `settime <UTC>` + timezone over serial on connect. No Wi-Fi needed.
- **NTP** via a boot-only Wi-Fi window (then tear down before Thread) — the only safe
  way to use Wi-Fi on the C6.
- Track time internally as **Matter-epoch microseconds + `esp_timer` elapsed**, not
  the C library clock (which isn't wired to anything). Convert with
  `MATTER_EPOCH_OFFSET_S = 946684800` (Unix−Matter epoch).

## 9. The web dashboard pattern (Web Serial) — keep it additive

A browser dashboard is a great fit and **does not require touching the Matter core**:
- Add a **dedicated JSON channel on the native USB-Serial/JTAG port** (a separate
  FreeRTOS task). **Newline-delimited JSON**, one object per line. Console stays on
  the UART bridge — untouched.
- Frame types: `hub` (status snapshot), `telemetry` (per-device), `ack`, `event`
  (rebooting / device_paired / device_removed / …). Commands are plain text reusing
  the console handler (`get`, `pair`, `remove`, `settime`, …).
- Consume it in a **zero-build browser app** via the **Web Serial API**:
  - Chromium only (Chrome/Edge/Opera), **secure context** (`http://localhost` or
    HTTPS) — serve it; `file://` won't work.
  - Auto-detect via `getPorts()` filtered on VID/PID `303A:1001`; `requestPort()`
    (filtered) only on first grant.
  - **Web Serial teardown must cancel the reader and drain the pipe before
    `close()`**, or the next open throws "port in use".
  - **Opening the native USB port resets the ESP32** (DTR/RTS) — so a reboot on
    connect/refresh is expected; the app should auto-reconnect and the hub should
    boot fast. Don't fight it.
  - Serve with **no-cache headers** + **dual-stack** (bind ::1 *and* 127.0.0.1) —
    Windows resolves `localhost`→::1 first; a v4-only server stalls ~2 s/request, and
    stale cached JS causes phantom bugs.
- History/charts: keep in the browser (IndexedDB) — the hub holds only latest values.

## 10. Build / flash gotchas

- **Flash fails with "Invalid head of packet (0x45)" / "Packet content transfer
  stopped"** when a browser tab is holding the native USB port and its reconnect
  logic keeps resetting the chip mid-flash. **Close the dashboard tab before
  flashing**, or flash with `esptool --no-stub -b 115200 write_flash @flash_args`.
- To reboot the device between modes without a factory reset: `esptool --before
  default_reset --after hard_reset flash_id` (a one-shot commission flag is cleared
  at boot, so a plain reset returns to operating mode).
- Factory reset the hub: hold BOOT ≥5 s, or `reset` on the console (erases NVS —
  wipes paired devices *and* the Thread dataset). NB: resetting the *hub* does **not**
  decommission the *end devices* (they hold the old fabric); reset those separately.
- Reading a serial port for tests without resetting the chip: open with `dtr=False,
  rts=False` before `open()` (pyserial).

## 11. Anti-patterns (things that cost us dearly)

- ❌ Restructuring commissioning to add a UI feature (reboot-free pairing, always-on
  BLE) → radio contention → flaky pairing. **Additive only.**
- ❌ Removing the Wi-Fi/NTP window "because we get time from the host" *without*
  checking it's on the commissioning path (it wasn't the bug, but the churn hid the
  real regression).
- ❌ Remove that only clears NVS (leaves devices commissioned + orphaned on Thread).
- ❌ Assuming a discovery/BLE failure is a firmware bug — it's usually the device not
  advertising, RF range, or a degraded device. **Diff against a known-good build on a
  clean network before deep firmware debugging.**
- ❌ Trusting the browser's normal reload after editing served JS (stale cache). Use
  no-cache serving + hard reload.

## 12. Suggested V1.0 architecture (clean start)

1. **Core (sacred): controller + Thread + commissioning + subscriptions.** Get this
   rock-solid first, on a clean Thread network, with a known-good device.
   - Reboot-based mode isolation (operating ⇄ commissioning).
   - Auto-commission on the pair-triggered boot (hands-off), with a watchdog +
     reboot-to-operating on failure.
   - Remove = `unpair_device` + `ShutdownSubscriptions` (proper decommission).
   - Time from host (serial `settime`) — skip Wi-Fi unless standalone NTP is required.
2. **Additive dashboard layer:** the COM7 JSON channel + a Web Serial browser app.
   Never let it reach into the core.
3. **Discipline:** commit/tag a working baseline *before* each feature; if pairing
   breaks, `git stash` and re-flash the last good tag to isolate firmware vs
   environment.

---

## Appendix: reference facts

- Matter controller fabric index for the single fabric: **1**.
- Attestation override needed for non-DCL devices (IKEA etc.): accept `err 101 (PAA
  not found)`.
- No commissioning stop/cancel API — only `unpair_device(node_id)`.
- `ShutdownSubscriptions(FabricIndex, NodeId)` (from `app/InteractionModelEngine.h`)
  tears down a subscription at runtime.
- USB-Serial/JTAG driver: `usb_serial_jtag_driver_install` with small tx/rx buffers
  (heap); `usb_serial_jtag_read_bytes` / `write_bytes` with short timeouts (only
  drains when a host is attached).
- Reset reasons of interest: `esp_reset_reason()` — 1 POWERON, 3 SW, 4 PANIC,
  9 BROWNOUT, 11 USB, 12 JTAG.

*Prior art: the ALPSTUGA Hub (v1.2.1) — a working reference implementation of all of
the above.*
