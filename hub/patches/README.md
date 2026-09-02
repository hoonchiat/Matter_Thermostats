# Local esp-matter patches — Matter Hub V1.0

`esp_matter_local.patch` contains **local modifications to the vendored
esp-matter component** (`managed_components/espressif__esp_matter/`). These live
outside our own source tree, so they are NOT captured by a normal source commit
and would be **lost if `managed_components` is ever re-downloaded** by the IDF
Component Manager. Re-apply this patch after any such refresh.

## What each hunk fixes

| File | Fix | Why |
|------|-----|-----|
| `esp_matter_controller_client.h` / `.cpp` | `set_check_in_delegate()` hook | Upstream hard-wires `DefaultCheckInDelegate` (which only logs). Needed so the app can re-subscribe on an ICD check-in → **battery devices recover after a hub reboot with no user action**. |
| `platform/ESP32/nimble/BLEManagerImpl.cpp` | NULL-check `peer` after `peer_find()` in `SendWriteRequest` | `peer_find()` returns NULL once the BLE link is torn down post-PASE; a late ack-timeout then dereferenced NULL → **`Guru Meditation` load fault (MTVAL 0x8), hub rebooted mid-commission**. Intermittent race that blocked reliable commissioning. |
| `platform/ESP32/nimble/peer.c` | NULL guard in `peer_svc_find_uuid` | Defence-in-depth for the same crash (`&peer->svcs` == 0x8 when peer is NULL). |
| `lib/dnssd/Resolver_ImplMinimalMdns.cpp` | `kMaxThreadPeerHints` 4 → 12 | The Thread peer-hint table (one entry per node) was sized 4; at 5+ devices it overflowed and starved a working device offline. Must be ≥ `MAX_DEVICES` (7) + staging headroom. |

> NOTE: the vendored esp-matter also carries a *pre-existing* ALPSTUGA patch
> (`setup_commissioner(bool enable_ble)`), so this component must be vendored, not
> used pristine from the registry. This patch is on top of that baseline.

## How to build (Windows)

The project path contains a space (`Matter Hub V1.0`), which breaks the CHIP/
ESP-IDF build. Build from a **space-free path**:

1. Copy the project (or just work) under e.g. `C:\mh`.
2. Provide `managed_components/` (copy from a known-good build, or let the IDF
   Component Manager fetch per `main/idf_component.yml`).
3. Apply this patch from the component root:
   ```
   cd managed_components/espressif__esp_matter
   git apply /path/to/patches/esp_matter_local.patch    # or: patch -p1 < ...
   ```
4. Build: `set "MSYSTEM=" & call C:\esp\v5.4.4\esp-idf\export.bat & idf.py -C C:\mh build`
   (clearing `MSYSTEM` is required when invoking from Git Bash so export.bat runs).
