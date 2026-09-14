# Matter Provisioning Station — `provisioning-tool.html`

A self-contained browser page (desktop **Chrome/Edge**, offline) that turns each
identical unit — ESP32-C6 **Light** / **Button**, or nRF52840 **Temp Sensor** — into a
uniquely-commissionable Matter device: it issues a per-device **discriminator + passcode
+ serial**, keeps a device database, and prints a QR label. No toolchain, no Python, no
per-device build step.

This complements [`../flash-tool`](../flash-tool): the flash tool writes plain firmware;
the provisioning station adds a **unique identity + label**.

## How the identity gets onto the board

Every device family runs the same custom `CommissionableDataProvider` idea: it reads a
fixed 64-byte blob `{magic, discriminator, passcode, serial}` from a flash partition and
derives the SPAKE2+ verifier **on-device** from the passcode + a shared salt — so the
browser only writes three small values, no crypto. The built-in **test DAC**
(VID 0xFFF1 / PID 0x8000) is kept (the hub accepts it). A blank partition falls back to
the test defaults (discriminator 3840, passcode 20202021), so an un-provisioned board
still boots.

The blob is byte-identical across families; only *how it reaches flash* differs:

| Family | Firmware | Partition | Written by the tool as |
|---|---|---|---|
| Light / Button (ESP32-C6) | `main/provisioning.cpp`, `CONFIG_CUSTOM_COMMISSIONABLE_DATA_PROVIDER=y` | `fctry` @ `0x3e0000` | an **esptool serial write** |
| Temp Sensor (nRF52840) | `src/provisioning.cpp` (registered via `mPreServerInitClbk`) | `factory` @ `0x0f3000` | a tiny **identity UF2** you drag on |

Because the nRF flashes over UF2 (USB mass-storage), not serial, the tool can't write it
directly — instead it generates a per-device **identity `.uf2`** (16 blocks targeting
`0x0f3000`, nRF52840 family `0xADA52840`) that you drop onto the board's bootloader
drive. The app itself is a separate, shared UF2 (also embedded here, and in `flash-tool`).

## Using it

1. **Open the page** in desktop Chrome/Edge. For the smoothest database experience,
   serve the folder and open it over localhost:
   ```
   cd provisioning-tool
   python -m http.server 8777      # then open http://localhost:8777/provisioning-tool.html
   ```
   (Double-clicking the file also works, but the live database file needs localhost.)
2. **Database** — click *New database…* (or *Open…*) and pick a `matter-devices.json`.
   It auto-increments serials, discriminators and passcodes and remembers every device.
   (Browsers without the File System Access API get *Export/Import JSON* + a local cache.)
3. **Next device** — choose Light, Button, or Temp Sensor. The serial, discriminator and
   passcode are filled in automatically (editable; *↻ New passcode* re-rolls). The unit
   name goes on the label.
4. **Write it:**
   - **Light / Button (serial)** — plug the board in, *Select port & connect*, then
     *Flash & provision*. Leave *Provision only* unchecked for a fresh board (writes
     firmware + identity); check it to re-provision a board that already has the app
     (writes only the `fctry` partition, preserving the app and any pairing).
   - **Temp Sensor (UF2)** — double-tap **RESET** so the board mounts as a drive. For a
     fresh board, *Download app .uf2* and drag it on once. Then *Generate identity .uf2 &
     label* and drag that on — it writes just the `factory` partition, so re-provisioning
     never touches the app or its pairing.
5. On success the device is recorded, the counters advance, and a **label PNG downloads
   automatically**, named `<unit>_<serial>.png` — unit name, serial, passcode, the
   11-digit manual pairing code, and the Matter QR.

## Files & rebuilding

- `prov-template.html` — the UI + all logic (partition blob, Matter QR / manual code /
  Verhoeff, database, label canvas). The part you'd edit.
- `esptool.mjs` — the esptool-js flashing engine.
- `qrcode.js` — the QR renderer (`qrcode-generator`, MIT).
- `build.ps1` — embeds esptool + the QR lib + the current light/button binaries and the
  nRF temp-sensor app UF2 (`C:\mhtemp\nrf_temp-nrf52840-supermini.uf2`) into
  `../provisioning-tool.html`.

After `idf.py build` in `mhlight`/`mhbutton` (or `west build` in `mhtemp`), regenerate
the page:
```
cd provisioning-tool/provisioning-tool-src
powershell -ExecutionPolicy Bypass -File .\build.ps1
```

## Notes

- Codes are verified against the canonical Matter test vector (discriminator 3840 /
  passcode 20202021 → QR `MT:Y.K9042C00KA0648G00`, manual `34970112332`).
- The manual pairing code encodes only the 4-bit short discriminator, so for >16 units
  some short discriminators repeat — harmless, the passcode still disambiguates and the
  QR carries the full 12-bit discriminator.
- Keep the `matter-devices.json` — it's your record of every passcode/discriminator
  issued. The passcodes are secrets printed on the labels.
