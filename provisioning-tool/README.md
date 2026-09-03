# Matter Provisioning Station — `provisioning-tool.html`

A self-contained browser page (desktop **Chrome/Edge**, offline) that turns each
identical ESP32-C6 **Light** or **Button** into a uniquely-commissionable Matter
device: it writes a per-device **discriminator + passcode + serial** into the board's
`fctry` partition, keeps a device database, and prints a QR label. No toolchain, no
Python, no per-device build step.

This complements [`../flash-tool`](../flash-tool): the flash tool writes firmware to any
of the four projects; the provisioning station adds a **unique identity + label** to
the two accessories.

## How the identity gets onto the board

The light/button firmware runs a custom `CommissionableDataProvider`
(`main/provisioning.cpp`) that reads `{discriminator, passcode, serial}` from the
`fctry` flash partition (`0x3e0000`) and derives the SPAKE2+ verifier on-device — so
the browser only writes three small values, no crypto. The DAC stays the built-in test
DAC (the hub accepts it). A board with a blank `fctry` falls back to the test defaults
(discriminator 3840, passcode 20202021), so an un-provisioned board still boots.

**The firmware must be built with `CONFIG_CUSTOM_COMMISSIONABLE_DATA_PROVIDER=y`**
(already set in `rgb-light` and `button`). The binaries embedded here already have it.

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
3. **Next device** — choose Light or Button. The serial, discriminator and passcode are
   filled in automatically (editable; *↻ New passcode* re-rolls). The unit name goes on
   the label.
4. **Connect & flash** — plug the board in, *Select port & connect*, then *Flash &
   provision*. Leave *Provision only* unchecked for a fresh board (writes firmware +
   identity); check it to re-provision a board that already has the app (writes only the
   `fctry` partition, preserving the app and any pairing).
5. On success the device is recorded, the counters advance, and a **label PNG downloads
   automatically**, named `<unit>_<serial>.png` — unit name, serial, passcode, the
   11-digit manual pairing code, and the Matter QR.

## Files & rebuilding

- `prov-template.html` — the UI + all logic (partition blob, Matter QR / manual code /
  Verhoeff, database, label canvas). The part you'd edit.
- `esptool.mjs` — the esptool-js flashing engine.
- `qrcode.js` — the QR renderer (`qrcode-generator`, MIT).
- `build.ps1` — embeds esptool + the QR lib + the current light/button binaries into
  `../provisioning-tool.html`.

After `idf.py build` in `mhlight`/`mhbutton`, regenerate the page:
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
