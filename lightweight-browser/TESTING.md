# Testing LiteBrowser on your hardware

About 15 minutes, from a fresh device to your React app running in kiosk mode. You need
the device on the network, a way to run commands on it (SSH or a keyboard), and
`sudo` rights.

## 1. Get the browser onto the device

**Option A: download a prebuilt binary (quickest).**

1. Open the repository's **Actions** tab on GitHub, choose the **LiteBrowser** workflow,
   and open the latest green run on branch `claude/wonderful-hawking-y5yozw`.
2. Under **Artifacts**, download the one for your device's CPU. `uname -m` on the
   device tells you which:
   - `aarch64` → `litebrowser-linux-arm64` (Raspberry Pi 4/5 with 64-bit OS, most ARM boards)
   - `x86_64` → `litebrowser-linux-x86_64` (Intel/AMD mini-PCs, panel PCs)
3. Copy it to the device and unpack it. GitHub wraps it in a zip:

   ```sh
   scp litebrowser-linux-arm64.zip user@DEVICE:     # from your PC
   ssh user@DEVICE
   unzip litebrowser-linux-arm64.zip && tar xzf litebrowser-linux-arm64.tar.gz
   cd litebrowser
   sudo apt install libwebkit2gtk-4.1-0             # the only runtime dependency
   ```

The binaries need glibc 2.35 or newer: Debian 12 / Raspberry Pi OS Bookworm, Ubuntu
22.04, or later. A 32-bit OS (`armv7l`) has no prebuilt binary; use option B.

**Option B: build on the device.**

```sh
sudo apt install git libwebkit2gtk-4.1-dev libgtk-3-dev build-essential
curl --proto '=https' -sSf https://sh.rustup.rs | sh     # then open a new shell
git clone -b claude/wonderful-hawking-y5yozw https://github.com/hoonchiat/Matter_Thermostats
cd Matter_Thermostats/lightweight-browser
cargo build --release            # roughly 10–20 minutes on a Raspberry Pi 4
cp target/release/litebrowser .
```

## 2. Check the device

```sh
sh tools/check-device.sh
```

It lists the CPU, OS, whether WebKitGTK is installed, the display session (X11 or
Wayland), the screen's native resolution, and touch screens. Fix any `[FAIL]` before
going on. Note in particular:

- **Screen native mode `1920x1080`**: a landscape panel mounted sideways. Rotate the
  display and the touch input first; see "Portrait screens" in the README.
- **`1080x1920`**: natively portrait, nothing to rotate.
- **No touch screen detected**: check the touch cable (many panels use a separate USB
  cable for touch).

## 3. Run the hardware test page

Start the browser **on the device's screen**. If you're logged in on the device itself,
just run it. Over SSH, tell it which screen to use first:

```sh
# X11 desktop (Raspberry Pi OS before Bookworm, most Ubuntu/Debian installs with Xorg):
export DISPLAY=:0
# Wayland desktop (Raspberry Pi OS Bookworm, GNOME):
export XDG_RUNTIME_DIR=/run/user/$(id -u) WAYLAND_DISPLAY=wayland-1   # ls $XDG_RUNTIME_DIR for the name
```

Then:

```sh
./litebrowser --fullscreen --toolbar-bottom examples/hardware-test.html
```

On the touch screen, work through the page:

| Section | Pass when | If not |
|---|---|---|
| 1. Screen | Window is about 1080 wide; orientation says *portrait* | Display isn't rotated. README: Portrait screens |
| 2. Touch | A circle appears **under** your finger, and the four corner squares turn green | Offset/mirrored: touch not rotated to match. README: Portrait screens |
| 2. Touch | Input type shows **touch** (green) | Shows *mouse*: README: "Touch acts like a mouse?" |
| 2. Touch | "most at once" reaches 2 or more with several fingers | The panel or driver is single-touch; pinch zoom won't work |
| 3. Keyboard | Tapping each field slides the keyboard up; typing works; Enter in the last field shows *Submitted* | Run with `LITEBROWSER_DEBUG=1` and send the output |
| 3. Keyboard | The hide key (bottom right) closes it; tapping the field again reopens it | |
| 4. Gestures | Two-finger pinch zooms; after "Add history entry" a few times, an edge swipe goes back | WebKitGTK version or touch driver; not critical (the toolbar has Back) |
| 5. HTML5 | Everything ticked, except possibly the video codecs | Video codecs: `sudo apt install gstreamer1.0-libav gstreamer1.0-plugins-good` |
| 6. Performance | 50–60 fps | Below 40: try `--no-gpu` and compare |

Also tap the address bar in the bottom toolbar, type `example.com` on the on-screen
keyboard and press Enter. The page should load if the device has internet access.

To quit while fullscreen: press Alt+F4 with a keyboard attached, or run
`pkill litebrowser` over SSH.

## 4. Test your React app

From a production build (closest to deployment). On your PC, run `npm run build`, copy
the output folder (`dist/` for Vite, `build/` for Create React App) to the device, then:

```sh
./litebrowser --fullscreen --toolbar-bottom /path/to/dist
```

LiteBrowser serves the folder itself, so client-side routes and deep links work. Or use
your PC's dev server, for live reloading while you adjust the app for the screen:

```sh
npm run dev -- --host            # on your PC (Vite); note the "Network" address
./litebrowser --fullscreen --toolbar-bottom http://192.168.1.50:5173    # on the device
```

Check: text fields bring up the keyboard and React state updates as you type; scrolling
and swiping feel smooth; your routes navigate and the address bar follows them; nothing
important ends up hidden behind the keyboard.

## 5. Try kiosk mode, then run it at boot

```sh
./litebrowser --kiosk /path/to/dist          # or your app's URL
```

There's no toolbar, pointer or exit button: use `pkill litebrowser` over SSH to stop it.
To start the server later and confirm recovery, launch the browser first: it shows
"Can't reach this page" and reloads by itself once the app answers.

When you're happy, install it as a boot service. See "Running as a kiosk at boot" in the
README and `examples/kiosk/litebrowser-kiosk.service` (copy the binary to
`/usr/local/bin/` first).

## If something goes wrong

```sh
LITEBROWSER_DEBUG=1 ./litebrowser examples/hardware-test.html 2>&1 | tee litebrowser.log
journalctl -u litebrowser-kiosk -b           # when running as the boot service
```

Please send back:
- the output of `sh tools/check-device.sh`;
- which rows of the step 3 table failed;
- `litebrowser.log`;
- a photo of the screen if the layout looks wrong.
