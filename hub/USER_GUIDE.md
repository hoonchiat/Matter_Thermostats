# Matter Hub V1.0 — User Guide (v1.8)

The hub is an ESP32-C6 Matter **controller** + Thread **Border Router**. It
commissions Matter-over-Thread devices and gives you two independent ways to
operate them:

| Interface | Port | Baud | Format | Best for |
|-----------|------|------|--------|----------|
| **Console** | UART0 (`COM12`) | 115200 | free-text lines + human output; also carries `ESP_LOG` | interactive use, a terminal |
| **JSON protocol** | native USB-Serial-JTAG (`COM7`) | 115200 | NDJSON (one compact JSON object per line) | scripts, GUIs, a web dashboard |

Both run at once and are serialised internally (a command mutex), so a script on
COM7 and a person on COM12 never corrupt shared state. The console also emits log
output; the JSON port is **clean** (no logs) except for a one-time ROM/bootloader
banner at power-up.

> Port note: `COM7`/`COM12` are the numbers on this build's PC. "COM7" is the
> connector wired directly to the C6 (labelled **USB** on the DevKit); "COM12" is
> the CP2102 USB-UART bridge (labelled **UART**). Flashing uses COM12.

**Contents:** [1. Features](#1-feature-list) · [2. Connecting](#2-connecting) ·
[3. Console reference](#3-console-reference-com12) ([commissioning](#31-commissioning--devices) ·
[inspector](#32-inspector-readcontrol-a-device) · [bindings](#33-bindings-device--device) ·
[logic](#34-value-logic-automation) · [scheduling](#35-scheduling--calendar) ·
[status/config](#36-status-config-diagnostics)) ·
[4. JSON protocol](#4-json-protocol-reference-com7) ([queries](#41-queries-structured-results) ·
[actions](#42-actions-structured-ack--error) · [rebooting](#43-rebooting-actions) ·
[device I/O](#44-live-device-io-async)) · [5. Workflows](#5-common-workflows) ·
[6. Limits](#6-specifications--limits) · [7. Caveats](#7-notes--caveats)

---

## 1. Feature list

- **Commissioning** — pair one device (`pair`) or several in one go (`pairlist`);
  BLE-isolated, reboot-based (FOUNDATION §4/§5). `blescan` finds devices in pairing
  mode. `remove` decommissions.
- **Inspector** — walk any device: `devices`, `tree`, `ep`, `cluster`, `read`,
  `write`, `invoke`, `scan`; `on`/`off`/`toggle` shortcuts.
- **Device-to-device binding** — `bind` a button to a plug: hub-mediated relay for
  event-only switches (e.g. IKEA BILRESA) or native Matter binding for capable
  sources.
- **Value logic (automation)** — rules that watch sensor values (temp/humidity/
  co2/pm25) with hysteresis, boolean **AND/OR/XOR/NOT**, one level of **( )**
  grouping, and drive OnOff targets; **track** (auto-inverse) or **oneshot**.
- **Time scheduling** — up to **4 schedulers**, each a weekly (Mon–Sun) + holiday
  day-schedule; a shared **calendar** of public holidays (DD/MM, `**` wildcards);
  logic can use `schedule <n>` and `calendar` as conditions.
- **Live status / sensors** — one device or all, over either interface.
- **Config backup / restore** — the whole config (devices + bindings + logic +
  schedule + calendar) as JSON; restore re-pairs devices by identity; `restore wipe`
  factory-resets first. Enables hub-to-hub cloning.
- **Labels, timezone, persistent debug, heap diagnostics.**

---

## 2. Connecting

- **Console:** open `COM12` at 115200, 8N1. No DTR/RTS toggling (that reboots the
  hub). Type a command, press Enter.
- **JSON:** open `COM7` at 115200. Send one JSON object per line ending in `\n`;
  read one JSON object per line back. Each request may carry an `"id"` which is
  echoed in the response so you can correlate.

Minimal Python client for COM7:

```python
import serial, json
s = serial.Serial('COM7', 115200, timeout=3)
def call(obj):
    s.write((json.dumps(obj, separators=(',', ':')) + '\n').encode())
    return json.loads(s.readline())
print(call({"id": 1, "cmd": "status"}))
```

---

## 3. Console reference (COM12)

Every command is one line. `<n>` is a 1-based device number (see `devices`); `#n`
is the same in logic/bind. `[..]` = optional.

### 3.1 Commissioning & devices
| Command | What it does |
|---|---|
| `payload <MT:...\|code>` | set the onboarding code for the NEXT device |
| `pin <PIN> <disc>` | set manual credentials (discriminator 0–4095) |
| `blescan [secs]` | scan BLE for devices in pairing mode (discriminator + VID/PID) |
| `pair` | add ONE device using the set payload/pin (reboots into commissioning) |
| `pairlist <c1> <c2> ...` | batch-pair several devices, then print a paired/failed report |
| `remove <n>` | decommission device slot `<n>` (unpair + reboot) |
| `name <n> [label]` | set a device label (empty clears); shown as "label (name)" everywhere |
| `reset` | factory reset (erase NVS + Matter fabric) + restart |

Examples:
```
payload MT:Y.K9042C00KA0648G00
pair
pairlist 22089934501 25748432181 02259336520
name 4 Kitchen Plug
remove 3
```

### 3.2 Inspector (read/control a device)
| Command | What it does |
|---|---|
| `devices` (or `list`) | list paired devices: slot, payload, name/label, VID/PID, online state |
| `device <n>` | one device's detail |
| `tree <n>` | full endpoint → cluster tree (from the cached enumeration) |
| `ep <n> <ep>` | one endpoint's clusters |
| `cluster <n> <ep> <cl>` | live-read a cluster's attributes/commands |
| `read <n> <ep> <cl> <attr>` | live-read one attribute |
| `write <n> <ep> <cl> <attr> <value>` | write an attribute (esp-matter JSON value) |
| `invoke <n> <ep> <cl> <cmd> [args]` | invoke a command (args default `{}`) |
| `scan <n>` | re-enumerate a device |
| `on\|off\|toggle <n> [ep]` | OnOff shortcut (endpoint auto-found) |

Examples:
```
devices
tree 4
read 4 1 0x0006 0x0000
on 4
invoke 4 1 0x0006 0x02
write 1 0 0x0028 0x0005 {"0:STR":"MatterHub"}
```

### 3.3 Bindings (device → device)
| Command | What it does |
|---|---|
| `bindable <n>` | show how `<n>` can bind (source/target roles per endpoint) |
| `bind <s> <sep> <d> [dep] [on\|off\|toggle]` | bind source endpoint → dest OnOff (auto native/hub; default `toggle`) |
| `bindings [n]` | list hub-mediated rules (and `<n>`'s native table if given) |
| `unbind <idx>` | remove hub rule `<idx>` |
| `unbind <s> <sep> <d>` | remove a native binding |

Examples:
```
bindable 3
bind 3 1 4 toggle        # BILRESA button ep1 -> toggle plug #4
bind 3 2 5 on            # button ep2 -> turn plug #5 on
bindings
unbind 0
```

### 3.4 Value logic (automation)
```
logic add <term> [and|or|xor <term>]... <on|off|toggle> #d [#d2..] [oneshot]
    <term> = <cond>  |  ( <cond> [and|or|xor <cond>]... )     (one group level)
    <cond> = [not] <sensor> [avg|min|max] #s.. <op> <val> [hyst <v>]
           | [not] schedule <n>        (scheduler 1..4 active now)
           | [not] calendar            (today is a public holiday)
    sensors: temp humidity co2 pm25    op: > >= < <=    (default mode: track)
```
| Command | What it does |
|---|---|
| `logic` (or `logic list`) | list rules + live sensor values |
| `logic add ...` | add a rule (see grammar) |
| `logic rm <idx>` | remove rule `<idx>` |
| `logic help` | print the grammar |

Examples:
```
logic add temp #1 > 28 on #4
logic add temp #1 > 28 hyst 1.0 off #4 oneshot
logic add co2 avg #1 #2 > 1250 on #5 #6
logic add temp #1 > 28 and co2 #1 > 1000 on #4
logic add temp #1 > 28 and schedule 1 on #5
logic add calendar and ( temp #2 > 25 or co2 #2 > 10000 ) on #4
logic add not schedule 2 and humidity #1 < 40 on #4
logic rm 0
```
- **track** (default): the target follows the condition (on when true, inverse when
  false), re-asserted every 120 s. **oneshot**: fires once on the rising edge.
- Hysteresis stops chatter: `temp #1 > 28` (default hyst 0.5) turns on at 28.0 and
  off at 27.5.
- A rule only acts once **all** its conditions have data; `schedule`/`calendar`
  read **false** until the clock is set.

### 3.5 Scheduling & calendar
| Command | What it does |
|---|---|
| `sched` | list all 4 schedulers: cal-control flag, each day's events, live on/off |
| `sched <n>` | list scheduler `<n>` (1..4) |
| `sched <n> <day> add HH:MM HH:MM` | add an event; `<day>` = `mon`..`sun` or `holiday`; 2nd time = last ON minute |
| `sched <n> <day> rm <idx>` | remove an event |
| `sched <n> cal on\|off` | enable/disable calendar (holiday) control for scheduler `<n>` |
| `calendar` | list public holidays + whether today matches |
| `calendar add DD/MM` | add a holiday date (`**` = any day or any month) |
| `calendar rm <idx>` | remove a holiday |
| `tz <±HH:MM>` | set/show the timezone offset (local = UTC + tz) |
| `settime YYYY-MM-DD HH:MM:SS [+HH:MM]` | set the clock (local time; add an offset to also set tz) |

Event semantics: `08:00 08:59` means **on at 08:00, off at 09:00** (the second
field is the last ON minute). Events on the same day must not overlap; max 4/day.

Examples:
```
tz +08:00
settime 2026-08-05 10:30:00 +08:00
sched 1 mon add 08:00 17:59         # weekdays 08:00 -> off 18:00
sched 1 tue add 08:00 17:59
sched 1 sat add 09:00 12:59
sched 1 holiday add 10:00 11:59     # different hours on public holidays
sched 1 cal on                      # honour the holiday schedule on holidays
calendar add 25/12                  # Christmas
calendar add 01/01                  # New Year
calendar add **/07                  # all of July (e.g. school break)
calendar add 01/**                  # the 1st of every month
sched
```

### 3.6 Status, config, diagnostics
| Command | What it does |
|---|---|
| `status` | mode, device count, radio, debug, next-pair creds |
| `dash` | one-shot live table of every device (values + reachability + last press) |
| `heap` | free / min-ever / largest-block + task stack headroom |
| `debug on\|off` | verbose logging (persists across reboots) |
| `backup` | print the whole config as one compact JSON line |
| `restore` | paste a backup JSON (end with a line `.`), re-pairs devices as needed |
| `restore wipe` | factory-erase first, then restore exactly from the JSON |

---

## 4. JSON protocol reference (COM7)

**Framing:** one compact JSON object per line, `\n`-terminated, both directions.

- Request: `{"id":<int>,"cmd":"<name>", ...args}` — `id` optional, echoed back.
- Success: `{"id":<int>,"ok":true,"result":{...}}` (queries) or `{"ok":true}` /
  `{"ok":true,"result":{"rebooting":true}}` (actions).
- Error: `{"id":<int>,"ok":false,"error":"<message>"}` — malformed JSON, unknown
  command, or a validation failure. Never crashes; never a partial NVS change.

Device references: `"dev"`/`"src"`/`"dst"` are 1-based device numbers; `status`'s
`"dev"` also accepts a payload string.

### 4.1 Queries (structured results)
| `cmd` | Args | Result |
|---|---|---|
| `ping` | — | `{"pong":true}` |
| `status` / `devices` | `{}` = all, or `{"dev":<n\|payload>}` | `{"devices":[...]}` (identity, sensors, online/age, rssi/lqi, battery) |
| `heap` | — | `{"free","min_ever","largest"}` |
| `schedule` / `sched_list` / `calendar_list` | — | `{"tz","schedulers":[...],"calendar":[...]}` |
| `logic_list` | — | `[{"idx","text","state"}]` |
| `bindings` | — | `[{"idx","src","src_ep","dst","dst_ep","action"}]` |
| `backup` | — | the full config object (devices/bindings/logic/schedule) |

Examples:
```json
{"id":1,"cmd":"ping"}
  -> {"id":1,"ok":true,"result":{"pong":true}}

{"id":2,"cmd":"status","dev":1}
  -> {"id":2,"ok":true,"result":{"devices":[{"slot":1,"node_id":"0x0000000200000001",
     "name":"ALPSTUGA air quality monitor","label":"","payload":"22089934501",
     "vid":4476,"pid":12289,"enumerated":true,"age_s":3,"online":true,"rssi":-37,
     "lqi":3,"sensors":{"temp":24.7,"humidity":53.0,"co2":892,"pm25":1}}]}}

{"id":3,"cmd":"status"}          # all devices
{"id":4,"cmd":"heap"}            # -> {"free":41064,"min_ever":5520,"largest":14592}
{"id":5,"cmd":"backup"}          # -> the whole config as result
{"id":6,"cmd":"logic_list"}
{"id":7,"cmd":"bindings"}
{"id":8,"cmd":"schedule"}
```

### 4.2 Actions (structured ack / error)
| `cmd` | Args | Notes |
|---|---|---|
| `logic_add` | `{"expr":"<console logic syntax>"}` | same grammar as `logic add` |
| `logic_rm` | `{"idx":<n>}` | |
| `bind` | `{"src","src_ep","dst","dst_ep"?,"action"?}` | action `on\|off\|toggle` (default toggle) |
| `unbind` | `{"idx":<n>}` | hub rule |
| `sched_add` | `{"n","day","start","end"}` | `day`=`mon`..`sun`/`holiday`; times `"HH:MM"` |
| `sched_rm` | `{"n","day","idx"}` | |
| `sched_cal` | `{"n","on":<bool>}` | calendar control for scheduler `n` |
| `calendar_add` | `{"date":"DD/MM"}` | `**` wildcard allowed |
| `calendar_rm` | `{"idx":<n>}` | |
| `tz` | `{"offset":"+08:00"}` or `{"offset":<minutes>}` | |
| `settime` | `{"iso":"2026-08-05 10:30:00 +08:00"}` | |
| `name` | `{"dev","label"}` | empty label clears |
| `payload` | `{"code":"<MT:..\|code>"}` | for the next `pair` |
| `pin` | `{"pin":<n>,"disc":<n>}` | |
| `debug` | `{"on":<bool>}` | |

Examples:
```json
{"id":10,"cmd":"logic_add","expr":"temp #1 > 28 and schedule 1 on #4"}
  -> {"id":10,"ok":true}

{"id":11,"cmd":"bind","src":3,"src_ep":1,"dst":4,"action":"toggle"}
{"id":12,"cmd":"sched_add","n":1,"day":"mon","start":"08:00","end":"17:59"}
{"id":13,"cmd":"sched_cal","n":1,"on":true}
{"id":14,"cmd":"calendar_add","date":"25/12"}
{"id":15,"cmd":"tz","offset":"+08:00"}
{"id":16,"cmd":"name","dev":4,"label":"Kitchen Plug"}
{"id":17,"cmd":"calendar_add","date":"31/13"}
  -> {"id":17,"ok":false,"error":"bad date (DD/MM, ** = any)"}
```

### 4.3 Rebooting actions
These respond **first** with `{"result":{"rebooting":true}}`, then reboot — the
connection drops; reconnect and poll `status` (and, for pairing, the commissioning
takes ~1–4 min per device).
| `cmd` | Args | Notes |
|---|---|---|
| `pair` | `{"payload":"<code>"}` | reboots into commissioning |
| `pairlist` | `{"payloads":["c1","c2",...]}` | batch commission |
| `remove` | `{"dev":<n>}` | decommission a device |
| `reset` | `{}` | factory reset |
| `restore` | `{"backup":{...},"wipe"?:<bool>}` | re-pairs by identity; `wipe` erases first |

Examples:
```json
{"id":20,"cmd":"pair","payload":"22089934501"}
  -> {"id":20,"ok":true,"result":{"rebooting":true}}
{"id":21,"cmd":"pairlist","payloads":["25748432181","02259336520"]}
{"id":22,"cmd":"remove","dev":3}
{"id":23,"cmd":"restore","backup":{ ...a prior backup result... }}
{"id":24,"cmd":"restore","backup":{ ... },"wipe":true}
```

### 4.4 Live device I/O (async)
These issue a live Matter operation and the response comes back when the device
answers (or an ~8 s timeout gives `{"ok":false,"error":"device I/O timeout"}`). One
device-I/O request is in flight at a time; a second gets `"device I/O busy"`.
| `cmd` | Args | Result |
|---|---|---|
| `read` | `{"dev","ep","cluster","attr"}` | `{"values":[{"ep","cluster","attr","value","text"}]}` (`value` is a number when scalar, else the decoded string) |
| `cluster` | `{"dev","ep","cluster"}` | same shape — a wildcard read of every attribute on the cluster |
| `invoke` | `{"dev","ep","cluster","command",  "args"?}` | `{"status","ep","resp"?}` — **note `command`** (the top-level key is `cmd`); `args` is an esp-matter JSON string |
| `onoff` | `{"dev","action","ep"?}` | `{"status","ep"}` — convenience OnOff `on`/`off`/`toggle` |
| `write` | `{"dev","ep","cluster","attr","value"}` | `{"sent":true}` — `value` is an esp-matter JSON string; the device's write status shows on the console log |
| `scan` | `{"dev"}` | `{"scanning":true}` — re-enumerate; read the tree back via `status`/console `tree` |

Examples:
```json
{"id":30,"cmd":"read","dev":1,"ep":1,"cluster":1026,"attr":0}
  -> {"id":30,"ok":true,"result":{"values":[{"ep":1,"cluster":1026,"attr":0,
     "value":2412,"text":"2412"}]}}          # temperature MeasuredValue = 24.12 C

{"id":31,"cmd":"cluster","dev":1,"ep":1,"cluster":1026}   # all attrs of the cluster
{"id":32,"cmd":"onoff","dev":4,"action":"toggle"}
  -> {"id":32,"ok":true,"result":{"status":0,"ep":1}}
{"id":33,"cmd":"invoke","dev":4,"ep":1,"cluster":6,"command":1}      # OnOff On
{"id":34,"cmd":"write","dev":1,"ep":0,"cluster":40,"attr":5,"value":"{\"0:STR\":\"Hub\"}"}
{"id":35,"cmd":"scan","dev":1}
```
Cluster/attribute ids are plain JSON numbers (decimal): temp `0x0402`=1026,
OnOff `0x0006`=6, BasicInfo `0x0028`=40.

**Still console-only:** `blescan` (commissionable BLE scan) and the live `dash`
sweep — use the console for those.

---

## 5. Common workflows

**Commission a few devices, label them (console):**
```
pairlist 22089934501 25748432181 02259336520
name 1 Living Room Air
name 4 Fan Plug
devices
```

**Fan follows temperature (console):**
```
logic add temp #1 > 28 hyst 0.5 on #4     # on at 28.0, off at 27.5, auto-tracks
```

**Office hours, holiday-aware (console):**
```
tz +08:00
settime 2026-08-05 09:00:00 +08:00
sched 1 mon add 08:00 17:59
sched 1 tue add 08:00 17:59
sched 1 wed add 08:00 17:59
sched 1 thu add 08:00 17:59
sched 1 fri add 08:00 17:59
sched 1 holiday add 00:00 00:00           # effectively off on holidays
sched 1 cal on
calendar add 25/12
calendar add 01/01
logic add schedule 1 on #4                # plug on only during office hours
```

**Air-quality + schedule + grouping (JSON):**
```json
{"cmd":"logic_add","expr":"schedule 1 and ( temp #1 > 26 or co2 #1 > 1000 ) on #4"}
```

**Read all sensors periodically (JSON):**
```python
while True:
    print(call({"cmd":"status"})["result"]["devices"])
    time.sleep(30)
```

**Clone one hub's config to another (JSON):**
```python
cfg = call({"cmd":"backup"})["result"]          # from hub A
callB({"cmd":"restore","backup":cfg,"wipe":True})  # onto hub B (re-pairs from payloads)
```

---

## 6. Specifications & limits

| Item | Limit / value |
|---|---|
| Paired devices | **7** |
| Sensors tracked | `temp` (°C), `humidity` (%), `co2` (ppm), `pm25` (µg/m³) |
| Logic rules | **12** |
| Terms per rule (top-level, gate-joined) | **4** |
| Conditions per term (one `( )` group) | **3** |
| Grouping depth | **one level** of parentheses |
| Source devices per condition (avg/min/max) | **4** |
| Target devices per rule | **4** |
| Operators / gates / aggregation | `> >= < <=` / `and or xor` / `avg min max` |
| Actions / modes | `on off toggle` / `track` (default), `oneshot` |
| Default hysteresis | temp 0.5, humidity 2.0, co2 50, pm25 5 |
| Logic re-evaluation | on each sensor report; time rules every ~30 s; safety re-assert every 120 s |
| Hub-mediated bindings | **12** |
| Schedulers | **4** (each: Mon–Sun + 1 holiday day-schedule) |
| Events per day-schedule | **4**, non-overlapping |
| Calendar holidays (shared) | **20** (`DD/MM`, `**` = any day and/or any month) |
| Time base | local = UTC + tz; schedule times are local `HH:MM` |
| Clock | set via `settime`; **not battery-backed** — lost on every reboot |
| JSON request line | up to ~1.2 KB |
| Ports / baud | console `COM12`, JSON `COM7`; both 115200, 8N1 |

---

## 7. Notes & caveats

- **Clock after reboot.** The hub has no RTC; after a reboot the time is unset and
  `schedule`/`calendar` conditions read **false** (fail-safe — nothing fires on a
  wrong time). Re-send `settime` at connect (the JSON port makes this easy for a
  host program).
- **Offline devices.** A device that has dropped off Thread shows `enumerated:false`
  / `online:false` (last-known identity kept). Logic/bind **targets must be
  enumerated** (`scan <n>` to re-enumerate); IKEA devices re-attach staggered over
  ~2–5 min after a hub reboot. Sleepy ICD devices (buttons) may take ~1 min.
- **Commissioning is reboot-isolated.** `pair`/`pairlist`/`restore` reboot into a
  BLE commissioning session and back; the hub is briefly offline. The JSON port is
  **not served during commissioning** (it starts only in operating mode) to protect
  the heap-tight commissioning window.
- **Restore re-pairs by identity.** Matter credentials cannot be exported, so
  `restore` re-pairs each backup device from its payload; bindings/logic follow the
  device by identity regardless of the slot it lands in. A device that does not
  re-pair has its rules **skipped and reported**. `restore wipe` erases the current
  fabric first (each device must be back in pairing mode to re-commission).
- **Heap.** Operating free heap is ~40 KB; watch it (`heap`) if you push near the
  device/rule limits.
- **Two interfaces, one truth.** Console and JSON call the same validated core, so
  a rule added on one shows up on the other; `backup` captures everything.

---

*Firmware: Matter Hub V1.0, branch `v1.8-dev`. Build from a space-free path
(`C:\mho`); flash on COM12. See `FOUNDATION.md` for architecture and `STATUS.md`
for current state.*
