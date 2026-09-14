# Matter Firmware Flasher — `flash-tool.html`

A single self-contained HTML page that flashes any of the project firmwares over USB,
straight from the browser:

- **Matter Hub** (ESP32-C6) — serial
- **Gateway Portal** (ESP32-S3) — serial
- **RGB Light** (ESP32-C6) — serial
- **Button** (ESP32-C6) — serial
- **Temp Sensor** (nRF52840 SuperMini) — **UF2 drag-drop**

The ESP32 firmwares flash over Web Serial with the embedded flashing engine
([esptool-js](https://github.com/espressif/esptool-js)). The nRF52840 uses a UF2
bootloader (USB mass-storage) that a browser can't write over serial — so for that
device the page hands you the embedded **`.uf2`** to download and drag onto the board's
bootloader drive. Everything is embedded — nothing is downloaded at runtime, works
offline.

```
flash-tool/
├── flash-tool.html     ← open this (built, ~11 MB, all firmwares embedded)
├── README.md           ← this file
└── flash-tool-src/     ← how it's rebuilt
    ├── flash-template.html   UI + flashing logic (the part you edit)
    ├── esptool.mjs           esptool-js bundle (self-contained)
    └── build.ps1             base64-embeds the binaries + esptool into the page
```

## Using it

1. Open **`flash-tool.html`** in **desktop Chrome or Edge** (double-click it).
   - Web Serial only exists in Chromium browsers — Firefox, Safari, and phones can't flash.
   - If the browser refuses serial access from a local file, serve it instead:
     ```
     cd flash-tool
     python -m http.server 8777
     ```
     then open `http://localhost:8777/flash-tool.html`.
2. **Choose firmware** — Hub (C6), Portal (S3), Light (C6), Button (C6), or Temp Sensor
   (nRF52840).
3. **ESP32 devices — Connect** — plug the board in via USB, click *Select port &
   connect*, pick its COM port. The tool reads the chip and confirms it matches (it
   *warns* but still lets you flash a mismatch).
4. **ESP32 devices — Erase & Flash** — full-chip erase, then writes the firmware. Don't
   unplug until it says done; the board reboots automatically.
   - A factory-fresh C6/S3 shows up as a COM port over its built-in USB. If it doesn't
     respond, hold **BOOT** while connecting, then release.
3'. **Temp Sensor (nRF52840) — UF2** — the serial steps are replaced by a UF2 panel:
   **double-tap RESET** (the board mounts as a USB drive, e.g. `NRF52BOOT`), click
   **Download .uf2**, then **drag the file onto that drive**. The board flashes and
   reboots. Needs the Adafruit nRF52 UF2 bootloader.

## Rebuilding after a firmware change

`build.ps1` embeds whatever is in each project's `build/` dir **in the original working
repos** — `C:\mho`, `C:\mhs3`, `C:\mhlight`, `C:\mhbutton` (not the monorepo copies) —
plus the nRF52840 UF2 from `C:\mhtemp\nrf_temp-nrf52840-supermini.uf2`. After
`idf.py build` (ESP32) or `west build` (nRF) in whichever project changed, regenerate
the page:

```
cd flash-tool/flash-tool-src
powershell -ExecutionPolicy Bypass -File .\build.ps1
```

That rewrites `../flash-tool.html`. The embedded version tags come from `git describe` in
each source repo, so the page shows which build it carries.
