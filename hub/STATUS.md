# Matter Hub — status & handoff (v1.8-dev)

New chat starting here: **read `FOUNDATION.md` first** (architecture + hard-won
gotchas), then this file for current state.

> **Branch `v1.8-dev`** — forked from **v1.7.0** (`bdcd857`), the last released
> tag. Developed in the space-free copy `C:\mho` and ported to the canonical repo.
> Build from a space-free copy (CHIP/ESP-IDF break on the spaced path); `C:\mho`
> is the current build dir. Rollback: `git checkout v1.7.0` for the last release,
> or the pristine v1.5 build in `C:\mh` (`idf.py -C C:\mh -p COM12 flash`).

## v1.8 — machine interface + scheduling (RELEASED, tag `v1.8.0`; hardware-verified on 5 devices)

Two workstreams on top of v1.7, both additive (commissioning/subscription paths
untouched, FOUNDATION §5/§11). New files: `main/schedule.{h,cpp}`, `main/comm.{h,cpp}`.

**Time scheduling (Part B, `main/schedule.{h,cpp}`).** Up to **4 schedulers**
(`SCHED_MAX`), each a weekly (Mon–Sun) + **holiday** day-schedule with ≤4
non-overlapping events/day; one shared **calendar** of ≤20 public-holiday dates
(`DD/MM`, `**` wildcard on either field). Event on/off is inclusive by minute
(`08:00 08:59` = on 08:00, off 09:00). Per-scheduler **calendar-control** flag: when
on, a holiday date makes the scheduler use its holiday day-schedule instead of the
weekday one. Local time = UTC + persisted `tz`; `settime` gains an optional offset
(`+HH:MM`/`Z`; without one the typed time is local). Fail-safe: `schedule`/`calendar`
read false until the clock is set this boot. Console: `sched`/`sched <n> <day>
add|rm`/`sched <n> cal on|off`, `calendar add|rm`/list, `tz`. NVS keys `K_SCHED`/
`K_CAL`/`K_TZ`; carried in backup/restore.

**Logic grammar expansion (Part C, `main/logic.{h,cpp}`).** The rule model is now
two-level: up to **4 terms** joined by top gates, each term a group of up to **3
conditions** joined by inner gates — one level of parentheses `X and (A or B)`.
Conditions gained a `kind`: sensor (as v1.5), `schedule <n>` (scheduler active now),
`calendar` (today is a holiday). Flat rules become single-condition terms so v1.5
semantics/left-to-right precedence are preserved. A ~30 s tick re-evaluates
time-referencing rules; sensor-report eval + 120 s reassert unchanged. render_rule /
`logic_to_json` / `logic_apply_json` updated for terms + kinds. Rule-table .bss grew
~1.3 → ~3.4 KB.

**USB JSON protocol (Part A, `main/comm.{h,cpp}`).** A machine interface on the
ESP32-C6 **native USB-Serial-JTAG port (COM7)**, separate from the UART0 console +
logs. **NDJSON** — one compact JSON object per line each way: req `{"id","cmd",…}` →
`{"id","ok",result|error}`. A `cmd_mutex` serialises command execution with the UART
console (`hub_console_exec`); both front-ends call the SAME validated core. comm_task
starts only in operating mode (keeps ~7 KB driver+task out of the commissioning heap).
- *Phase 1 — queries + actions (all structured):* `status` (1/all — identity,
  sensors, online/age, rssi/lqi, battery), devices, `schedule`/`logic_list`/
  `bindings`, `heap`, `backup` (config object direct); actions sched/calendar/tz,
  logic_add/rm, bind/unbind, name/payload/pin/debug/settime, and rebooting pair/
  pairlist/remove/reset/restore (respond `{"rebooting":true}` first). Reuses
  structured entry points added to schedule/logic/bindings + `hub_action_json` /
  `hub_stage_*` / `hub_backup_json` in main.
- *Phase 2 — async device I/O:* `read`/`cluster`(wildcard)/`invoke`/`onoff`/`write`/
  `scan`. Issues a CHIP controller command on the CHIP task; the response is written
  from the completion callback. One op at a time (busy flag); a SystemLayer timer
  (same thread as the done cb → no race) bounds each at ~8 s (`device I/O timeout`).
  read/cluster → `{"values":[{ep,cluster,attr,value,text}]}` (value = JSON number when
  scalar, else decoded string via `tlv_get_scalar`/`tlv_to_str`); invoke uses key
  `"command"` (NOT `"cmd"`). `blescan` + live `dash` remain console-only.

**Docs:** `USER_GUIDE.md` (console + JSON reference with examples, limits, caveats) +
`docs/user_guide.html` (self-contained render).

Verification (hardware, COM12 + COM7): tz/settime; sched add + overlap reject;
calendar wildcards + today-match; holiday override (weekday↔holiday day-schedule);
grouped logic `calendar and ( temp #2>25 or co2 #2>10000 )` parses/renders/fires;
`schedule 1 on #4` fires; JSON status(1/all), backup, every schedule/logic/binding
action, large responses, NDJSON errors, NVS persistence across reboot, both
interfaces concurrent; Phase 2 read (scalar+string), cluster wildcard, invoke status
0, write NodeLabel round-trip, ~8 s timeout on an offline device. Operating heap ~40 KB.

## v1.7 — batch pairing, device labels, config backup/restore (hardware-verified on 5 devices)

Additive only — commissioning/subscription paths untouched (FOUNDATION §5/§11).

- **Batch pairing (`pairlist <c1> <c2> …`).** A persisted queue (`pairq_t`,
  `PAIRQ_MAX=7`) drives commissioning across the per-device reboots, then prints a
  report of which payloads paired / failed. A batch-only PASE deadline
  (`PAIRQ_PASE_DEADLINE_MS=60000`) skips an absent device in ~1 min instead of
  burning the 240 s watchdog; `PAIRQ_MAX_ATTEMPTS=2`. Single-device `pair` is
  byte-identical when the queue is empty. Full fail-path verified (queue → attempt
  → PASE-skip → retry → FAILED → report → operating; no wedge/crash).
- **Device labels (`name <n> [label]`).** Per-device user label (`K_DEVLABEL`,
  `char[MAX_DEVICES][32]`); shown as "label (device name)" in devices/dash/logic/
  bindings so identical devices are distinguishable. Empty label clears.
- **Debug persists (`K_DEBUG`).** `debug on|off` now survives reboot (was reset
  each boot); restored via `apply_debug_level` at boot.
- **Config backup/restore (`backup` / `restore`) — JSON over the console.**
  `backup` prints the whole config `{devices, bindings, logic}` as ONE compact
  line (`cJSON_PrintUnformatted`) to copy off to a PC; `restore` pastes it back
  (end with a line `.`). Devices are referenced by an INDEX → payload (identity),
  not slot, so bindings/logic/labels are keyed to device identity. Matter creds
  (fabric/NOC/ICD) live in CHIP storage and can't be exported, so restore
  RE-PAIRS any backup device that isn't currently paired (seeds the v1.7 queue);
  after re-pairing drains, `restore_finalize()` resolves each backup index to the
  device's ACTUAL current slot by payload and applies labels/bindings/logic. A
  rule referencing a device that didn't re-pair is SKIPPED and reported by name;
  resolvable rules still apply (partial apply). **Pairing order is irrelevant** — a
  rule follows its device wherever it lands (proven by a reversed-index restore
  reconstructing the identical physical bindings). cJSON via the ESP-IDF `json`
  component (CMake `REQUIRES`). New NVS keys: `K_RESTOREJ` (pending JSON), `K_PAIRQ`.
- **`restore wipe`.** Factory-erases NVS first (hub keys + CHIP fabric — both in
  the default `nvs` partition, confirmed via `CHIP_*_NAMESPACE_PARTITION_LABEL`),
  re-seeds the JSON + full queue into the fresh NVS (survives the erase), then
  reboots to re-pair EVERY backup device and apply only the backup's config →
  exact match, no orphans. `nvs_flash_deinit → erase → init` (a bare erase leaves
  stale cached state); reboot immediately after the write-back. The destructive
  path is not auto-run on a live fabric (would orphan the paired devices);
  verified non-destructively that the erase is gated behind a valid paste
  (invalid JSON / `x`-cancel both return before any erase).

Verification (backup/restore, on 5 devices): clean compile; backup round-trip
(label+logic+bindings serialize as indices + keyword strings); same-device restore
+ identity remap; ghost re-pair path (enqueue → commissioning → PASE-skip → drain →
finalize) with named missing-device report + partial apply; reversed-index restore
proves order-independence; bad-input rejection (cancel / malformed / no-marker) with
no NVS change, no leak, no crash.

## v1.6 — RAM & performance refactor (all hardware-verified on 5 devices)

- **P1 enumeration diet.** Enumeration now issues one 8-path TARGETED read
  (every cluster's AttributeList 0xFFFB + AcceptedCommandList 0xFFF9 — both
  mandatory globals, so coverage is identical — plus DeviceTypeList, ep0
  PartsList, 4 Basic Info identity attrs) instead of the old triple-wildcard
  read of every attribute VALUE. ClusterInfo caches presence+counts only (8 B,
  was 152 B); EndpointInfo ~150 B (was ~2.4 KB) → ~29 KB heap back at 5 devices.
  `cluster <n> <ep> <cl>` prints cached counts then LIVE-reads the cluster
  (ids+labels+values). RESULTS: 0 partial enums, 0 defer retries, 0 PacketBuffer
  exhaustion during enum (was ~8-9/boot), heap ~49 KB during enum (was 10-28 KB),
  and mid-session `scan` completes (previously deferred 4x and cleared the tree).
  ENUM_MIN_HEAP left at 30000 (defers no longer trigger; lower only if needed).
- **P2 .bss trims (idf.py size: .bss 227000 → 221944, −5 KB).** bindings'
  ~2.3 KB native-bind ctx is calloc'd per operation (pointer==busy; callbacks
  guarded); logic rules store u8 SLOT indexes instead of u64 node ids (node ids
  are NODE_ID_BASE+slot ⇒ equivalent; rule table 3.4 → 1.3 KB; older NVS rule
  blobs dropped once — none were configured). Verified: rules add/fire/persist
  across reboot (`Loaded 3 logic rule(s)`, autonomous re-fire), hub binding
  rules intact.
- **P3 `heap` console command.** free / min-ever / largest-block + serial &
  subscribe task stack high-water. NEW OPERATING BASELINE: free ≈ 56-57 KB
  (v1.5: 36-39 KB) ⇒ **~+20 KB steady-state**; largest-block ~25 KB; serial
  stack min-free ~1.7 KB of 3.5 KB. REMAINING PRESSURE POINT: min-ever still
  dips to ~5-7 KB during the boot attach/CASE burst — much improved but boot
  remains the tight moment; watch it when adding devices 6-7.
  (subscription_task self-deletes; it nulls its handle so `heap` never queries
  a dangling TCB.)

Notes from verification: #2 ALPSTUGA was Thread-detached during the runs
(`operational discovery failed: 32`, device-side) — power-cycle it; the hub
re-attaches autonomously. IKEA devices re-attach staggered over ~2-5 min after
every hub reboot; `logic add` correctly rejects a target until it enumerates.

## v1.5 — in progress: user-defined value-triggered logic rules

Also in this cycle:
- **Subscription paths scoped to device capabilities (VERIFIED).** Each device's
  cap mask (which of temp/humid/co2/pm25/battery clusters it has) is derived on a
  COMPLETE enumeration (`hub_update_devcap`, guarded to skip partial trees to avoid
  under-scoping) and persisted (`K_DEVCAP`). `start_subscription_for` then builds
  only the paths a device needs (`devcap_wants`) — a plug carries liveness+events
  only, ALPSTUGA drops battery, BILRESA drops the 4 sensors. Applies from the boot
  AFTER first enumeration (caps persist during enum; reboots are frequent). Verified
  boot-to-boot: data flow fully intact (ALPSTUGA sensors, BILRESA cached battery,
  plug power all still reported) and heap improved enough that a device that
  PARTIAL-enumerated on the full-path boot enumerated fully on the scoped boot.
  CAVEAT: partial enumeration is REDUCED, not eliminated — the dominant boot-burst
  heap spike is the enumeration WILDCARD READ (reads all attributes; dipped heap to
  ~10 KB), not the subscription. So one device can still partial-enumerate under
  the burst (a different one each boot); a partially-enumerated plug can't be a
  logic/console OnOff target until `scan <n>` re-enumerates it (dash still live-
  reads it fine). Next lever if this matters: lighten/segment the enumeration read.
- **`debug off` now also silences the app/component INFO chatter** (hub/inspector/
  read_command/logic/dash tags dropped to WARN unless verbose) — the bulk of the
  console noise. `[hub] ...` state + warnings/errors still print. (Turning `debug
  on` floods the UART with OpenThread verbose logs — expected; keep it off normally.)
- **`MAX_DEVICES` 6 → 7.** NVS device-table migration handles BOTH directions now.
  A *raise* was always fine (smaller stored blob → low slots). A *shrink* used to
  REFUSE to load a larger stored blob and boot with 0 devices (→ commissioning
  mode, orphaning the paired devices) — hit this going 8→7. `nvs_load` is now
  **shrink-safe**: it loads the first MAX_DEVICES slots as long as no *paired*
  device sits in a dropped high slot (else it still refuses rather than truncate).
  Thread peer-hint patch bumped **8 → 12** (`kMaxThreadPeerHints`, must stay ≥
  MAX_DEVICES + headroom; see patches/README.md) — re-apply after any
  managed_components refresh.
- **`debug off` now silences the SRP peer-hint scan spam** (the `SRP hint scan` /
  `SRP peer hint` / `peer scan pass` ESP_LOGW lines are gated behind
  `g_debug_verbose`; `debug on` restores them).

New module `main/logic.{h,cpp}` (additive; commissioning path untouched). Where
v1.4 bindings relay a device EVENT, logic acts on device VALUES: watch a sensor
on one or more sources, optionally aggregate (avg/min/max), compare to a
threshold with a hysteresis deadband, and drive an OnOff target.

- **Multi-condition boolean (v1.5.1):** a rule is `<cond> [and|or|xor <cond>]...`
  (up to 4 conditions, folded left-to-right, equal precedence) with per-condition
  `not`. Each condition has its own hysteresis; a rule only acts once ALL its
  conditions have data. Fires to **multiple target devices**. `toggle` allowed in
  both modes (periodic re-assert is skipped for toggle so it doesn't self-flip).

- **Data source (KEY change):** every device now also subscribes to a fixed
  known-sensor set — temp `0x0402`, humidity `0x0405`, co2 `0x040D`, pm25
  `0x042A` (each MeasuredValue `0x0000`; ALPSTUGA has PM2.5, not PM10), added as **wildcard-
  endpoint** attribute paths on the EXISTING per-device subscription
  (`start_subscription_for`). A device lacking a cluster just never reports it,
  so a rule needs no re-subscribe to take effect. Values decode via new
  `tlv_get_scalar` (tlv_decode) and feed `logic_on_report` from
  `liveness_attribute_cb`. Sensor table in logic.cpp is the single source of
  truth for both the subscription paths and the rule keywords — add a row to
  support a new measurement.
- **Rules (NVS key `logicrule`, exact-size blob like `hubbind`):** hysteresis to
  stop flapping; per-rule **track** (auto-inverse when the condition clears) or
  **oneshot** (fire once on the rising edge). A 120 s tick re-asserts TRACK
  targets so a dropped command / manual change self-heals.
- **Console:** `logic` (list rules + live sensor values) · `logic add <cond>
  [and|or|xor <cond>]... <on|off|toggle> #d [#d2..] [oneshot]` where `<cond> =
  [not] <sensor> [avg|min|max] #s.. <op> <val> [hyst <v>]` · `logic rm <idx>` ·
  `logic help`.  Examples: `logic add temp #1 > 28 on #4` ·
  `logic add co2 avg #1 #2 > 1250 on #5 #6` ·
  `logic add temp #1 > 28 and co2 #1 > 1000 on #4 #5`.

### `dash` - live device overview (new console command, VERIFIED on hardware)
New module `main/dash.{h,cpp}`. `dash` live-reads every paired device **one at a
time** (sequential, so the packet-buffer pool isn't exhausted like a concurrent
fan-out) via a single multi-path read per device (temp/humidity/co2/pm25/OnOff/
battery, wildcard endpoint), with a **2.5 s per-device timeout** so an asleep/
offline device is shown `unreachable` instead of stalling the sweep. Prints a
one-shot table: present values + status (online/stale/offline, last-seen age,
RSSI/LQI, not-enum) + last button press (Switch events cached from
`event_report_cb` via `dash_on_event`). `tlv_get_scalar` extended to decode
booleans (OnOff). Reads are live per the user's choice (not the logic cache).
**Cached battery fallback:** dash also contributes PowerSource BatPercentRemaining
(0x2F/0x0C) to every device's subscription (`dash_sub_path*` appended in
`start_subscription_for`; `dash_on_report` from `liveness_attribute_cb`), so a
sleepy device's battery is cached passively (its subscription's initial report
carries it on re-attach). When a live dash read misses, it shows
`battery=N% (cached Ns ago)` — VERIFIED: BILRESA showed `battery=100% (cached 33s
ago)` while asleep, age refreshing as it re-reports. Additive; commissioning path
untouched. NOTE: this restores the per-subscription path count to 6 (undoing the
pm10 trim's saving); #5 plug still occasionally PARTIAL-enums under the known
enumeration-burst heap pressure - functional, but the lever to reclaim heap is the
same (scope sensor/battery paths to capable devices post-enumeration).

**Status: VERIFIED ON HARDWARE (2026-07-21, flashed to COM12, 5 devices).**
- ALPSTUGA sensor values stream live (`temp/humidity/co2/pm25`); `tree 1` confirms
  the real clusters match the SENSORS table (0x0402/0x0405/0x040d/0x042a) — cluster
  ids CONFIRMED. (pm10 0x042d removed from the set post-verify — ALPSTUGA has PM2.5,
  not PM10 — trimming one wildcard path per device.)
- Parser OK for single / `avg` / `and` / `not` / multi-target; `logic rm` OK; rules
  persist across the reflash.
- Full fire path proven: `logic add temp #1 > 20 on #4` fired `drive OnOff cmd 0x01
  status=0x00` and `read 4 1 0x0006 0` returned `OnOff = true` (plug physically on).
- `debug off` confirmed silencing the SRP peer-hint scan spam over 2+ scan cycles.

**Caveat — heap/PacketBuffer pressure:** during the post-reboot enumeration burst,
`PacketBuffer: pool EMPTY` + low heap (`free=15120..21032`) appeared and plug #5
needed several enum retries before succeeding (all recovered). This is the known
`project_alpstuga_heap` signature; v1.5 adds pressure (6 attr paths/subscription vs
2, now 5; MAX_DEVICES 6→7). Fine at 5 devices — WATCH HEAP when scaling to 6-7.
Already trimmed pm10 (one fewer wildcard path/device). Next lever if needed: scope
sensor paths to sensor-capable devices post-enumeration (plugs skip them).

## Released: v1.3.0 (tag `v1.3.0`, branch `master`) — verified on hardware

ESP32-C6 Matter controller + Thread BR with a **generic device inspector**.
Tested live with **5 devices**: 2× IKEA ALPSTUGA (air quality), 1× IKEA BILRESA
(dual button, sleepy LIT-ICD), 2× IKEA GRILLPLATS (plug).

Working & verified:
- Commission Matter-over-Thread devices; reboot-based operating⇄commissioning mode.
- Inspector: `devices` `device` `tree` `ep` `cluster` `read` `scan` `write` `invoke`,
  with cluster/attribute/command/event **name** resolution + generic TLV decode.
- `on`/`off`/`toggle <n> [ep]` (auto-locates OnOff endpoint) — controls the plugs.
- Event subscription (urgent) → BILRESA button presses arrive as events
  (InitialPress/ShortRelease/LongPress/MultiPressComplete; field 1 = press count).
- `blescan [secs]` — commissionable BLE scan (discriminator + VID/PID; passcode is
  never advertised).
- Status LED (GPIO8 WS2812): blue=boot, green=ready, amber=pairing.
- Commissioning watchdog (240 s) + proper decommission (unpair + ShutdownSubscriptions).
- Identity persistence (offline devices keep name/VID/PID). `MAX_DEVICES=6`.
- **Autonomous ICD recovery**: a sleepy battery device reconnects by itself ~2 min
  after a hub power-cycle — no user action. (Needs a clean re-pair to store the key.)

## Critical build/run notes (do not relearn the hard way)
- **Project path has a space** ("Matter Hub V1.0") → CHIP/ESP-IDF build breaks.
  Build from a space-free copy (this session used `C:\mh`). See `patches/README.md`.
- **Vendored esp-matter is patched** — `managed_components/` must be vendored, and
  `patches/esp_matter_local.patch` re-applied after any refresh (5 hunks: ICD
  check-in hook, BLE NULL-deref crash fix ×2, peer-hint table 4→8).
- Flashing needs COM12 free — close the Arduino serial monitor first.
- **Grep every hardware log for `Guru Meditation`/panic BEFORE theorising** — a
  BLE crash masqueraded as a dozen unrelated issues for most of v1.3's development.

## v1.4 — in progress

### Device-to-device binding (Path C / hybrid) — DONE for hub-mediated; native built-untested
New module `main/bindings.{h,cpp}` (additive; commissioning path untouched). Two
mechanisms, auto-selected per source by `bindable`:
- **Hub-mediated (VERIFIED on hardware):** for an event-only Generic Switch
  (0x003B) like BILRESA, which CANNOT do native binding (no Binding cluster; it is
  device-type GenericSwitch 0x000f, event-only by design — confirmed via `tree`).
  A persisted rule table (NVS key `hubbind`) maps a button's ShortRelease event to
  an OnOff on/off/toggle on the target; `bindings_on_event()` (hooked into
  `event_report_cb`) relays it. Tested BILRESA #3 ep1→ON / ep2→OFF plug #4:
  relay status=0x00, plug responds **instantly** while the hub is up.
  - **Live config (2026-07-20, runtime/NVS only — no code change):** reconfigured
    to two independent toggle buttons — BILRESA ep1 press → **toggle GRILLPLATS #4**,
    ep2 press → **toggle GRILLPLATS #5**. Verified on hardware: each press relayed
    OnOff Toggle (cmd 0x02) status=0x00. (Toggle is a mandatory OnOff command; both
    plugs support it.) To change: `unbind 0`/`unbind 1` then re-`bind` as desired.
- **Native Matter binding (compiled, NOT runtime-tested — no capable source device
  owned):** writes the source's Binding cluster (0x001E) table + the target's ACL
  (0x001F), both read-modify-write. ACL write ABORTS if it cannot faithfully
  round-trip existing entries, so the admin entry is never clobbered.

Commands: `bindable <n>` · `bind <s> <sep> <d> [on|off|toggle]` · `bindings [n]` ·
`unbind <idx>` (hub rule) / `unbind <s> <sep> <d>` (native).

NVS rule persistence VERIFIED (2026-07-20): after a reflash+reboot the rules
reloaded via `bindings_load` and relayed presses WITHOUT any re-bind (saw
`[hub] binding: #3 ep1 press -> on` straight from NVS). This persistence + the
existing 60s subscribe-retry (BILRESA reconnects ~62s after a hub reboot) is what
makes the switch "just work" quickly after a reboot.

Key finding: BILRESA is a LIT-ICD (IdleModeDuration=900s; wake gesture = RESET
button per UserActiveModeTriggerInstruction). Pressing the normal buttons does NOT
wake it to reconnect after a hub reboot — only its check-in or reset gesture does.
So the "slow to respond" is ONLY the post-hub-reboot reconnect (every COM12 open
reboots the hub via DTR/RTS); with the hub up, response is instant.

DEAD END — auto-scan-on-boot "reconnect probe" (TRIED & REVERTED 2026-07-20): added
a timer that read-probed not-yet-reported devices every 10s post-boot (the idea:
reads are safe to fire often, unlike subscribes, so nudge sleepy devices to
reconnect faster). Measured over 2 reboots: it NEVER beat BILRESA's ~62s self-wake
(the device is simply unreachable until it wakes; the probe reads to it just fail,
no `probe reached`), while the existing 60s retry already catches it at ~62s.
Worse, GRILLPLATS plugs are themselves slow to re-attach (not back at 92s), so the
probe kept firing useless reads at them and coincided with their CASESession
timeouts. Reverted fully. Lesson: reconnect latency for this hardware is bounded by
the device's ICD sleep cycle, not the hub's retry strategy — don't re-attempt.

Native path still needs a Binding-capable source device to validate.

### Other v1.4 candidates (nothing committed)
- Web dashboard over the native USB-JTAG JSON channel (FOUNDATION §9) — the big
  additive feature; console + Matter core stay untouched.
- Relocate the project to a space-free canonical path (retire the `C:\mh` copy).
- Optional: `settime` to silence TimeSync spam; surface Thread link RSSI/LQI
  (already sampled) in `devices`.
