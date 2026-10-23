# LiteBrowser

A lightweight web browser for **Linux touch screens**: kiosks, control panels, and
single-board computers such as the Raspberry Pi. It has full **HTML5** support and runs
modern **React** (and Vue, Svelte, Angular…) single-page apps.

LiteBrowser doesn't ship its own engine (Chromium alone is >100 MB). It is a thin Rust
shell around **WebKitGTK**, the engine behind GNOME Web, so the release binary is about
**1 MB**. WebKitGTK handles everything a React app needs: ES2023+, ES modules and import
maps, `fetch`, Web Workers, WebSocket, WebGL, IndexedDB/localStorage, `<video>`/`<audio>`,
CSS grid/`:has()`, the History API for client-side routing, and Touch/Pointer Events.

## Touch features

- **Finger-sized toolbar**: 48 px buttons and a 44 px address bar, with no hover-only UI
  and no double-tap-zoom delay.
- **Built-in on-screen keyboard**: slides up when a text field is tapped, in a page or in
  the address bar, and the page shrinks so the field stays visible. It has letters, a
  shift/caps lock key, two symbol pages, `.com`, cursor keys, and auto-repeating
  Backspace. It types through real input events, so **React controlled inputs**
  (`onChange`), `keydown` handlers and form submit on Enter all work. Autofocus during page
  load doesn't pop it up; only a tap does.
- **Gestures**: swipe from the screen edge for back/forward, and pinch to zoom (both
  provided by WebKitGTK).
- **Kiosk mode** (`--kiosk`): fullscreen, no toolbar, hidden pointer, no long-press
  context menu, no developer tools. It fills the screen even under a bare X server with no
  window manager.
- **Self-healing error page**: if the page can't load (for example because the app's
  server is still booting), it retries automatically with backoff and shows a large
  Retry button.

## Browsing features

- Address bar takes URLs, bare domains (`react.dev` → `https://`), dev servers
  (`localhost:5173` → `http://`), absolute file paths, or search terms (DuckDuckGo).
- **Local React builds just work**: `litebrowser my-app/dist` serves the folder on a
  private `127.0.0.1` port. Absolute asset paths (`/assets/index-abc.js`) resolve, ES
  modules aren't blocked by `file://` rules, and deep links like `/settings/profile` fall
  back to `index.html` (BrowserRouter-style routing).
- The address bar follows SPA route changes (`pushState`, `replaceState`, hash changes).
- Persistent profile (cookies, storage, cache) or `--incognito`.
- Pop-ups and `target="_blank"` links open in the current view.
- Follows the system light/dark theme. Developer tools via the `<>` button or F12.

## Build

Requires Rust 1.80+ and the WebKitGTK 4.1 development files.

```sh
sudo apt install libwebkit2gtk-4.1-dev libgtk-3-dev     # Debian / Ubuntu / Raspberry Pi OS
sudo dnf install webkit2gtk4.1-devel gtk3-devel         # Fedora

cd lightweight-browser
cargo build --release
sudo install target/release/litebrowser /usr/local/bin/
```

The target machine only needs the runtime library (`libwebkit2gtk-4.1-0`). Build on the
device itself, or on any machine with the same architecture (CI builds x86-64 and arm64).

## Usage

```
litebrowser [OPTIONS] [URL | FILE | FOLDER]

Display:
      --fullscreen         Start fullscreen, without window decorations
      --no-toolbar         Hide the toolbar (address bar and buttons)
      --hide-cursor        Hide the mouse pointer (touch-only screens)
      --kiosk              All three above, plus no context menu and no devtools
      --zoom <FACTOR>      Initial page zoom, e.g. 1.5 on small high-DPI panels
      --keyboard <MODE>    On-screen keyboard: on (default) shows it when a text
                           field is tapped; touch shows it for finger taps only;
                           off never shows it (use with a physical or system keyboard)

Browsing:
      --home <URL>         Page opened by the Home button (default: start page)
      --incognito          Do not persist cookies, storage or cache
      --no-serve           Open local files as file:// URLs instead of serving them

Troubleshooting:
      --no-gpu             Software rendering, for boards with broken GPU drivers
      --devtools           Open the developer tools on startup
```

Examples:

```sh
litebrowser                                     # start page, with toolbar
litebrowser --fullscreen react.dev              # fullscreen browsing
litebrowser --kiosk http://localhost:3000       # lock the screen to your app
litebrowser --kiosk --zoom 1.25 /opt/app/dist   # serve a React build, slightly larger
litebrowser --keyboard off localhost:5173       # desk testing with a real keyboard
```

### Running as a kiosk at boot

[`examples/kiosk/litebrowser-kiosk.service`](examples/kiosk/litebrowser-kiosk.service)
starts LiteBrowser under [cage](https://github.com/cage-kiosk/cage), a minimal Wayland
compositor that runs one fullscreen app and handles touch input. Edit the URL and user,
then:

```sh
sudo apt install cage
sudo cp examples/kiosk/litebrowser-kiosk.service /etc/systemd/system/
sudo systemctl enable --now litebrowser-kiosk
```

On X11 without a desktop, `xinit /usr/local/bin/litebrowser --kiosk URL -- -nocursor`
works too.

### Touch-screen tips

- **Touch acts like a mouse?** On X11, GTK needs XInput 2 to receive real touch events.
  Make sure `GDK_CORE_DEVICE_EVENTS` is not set. Wayland (cage, weston, GNOME, Phosh)
  delivers touch natively.
- **Wrong orientation or touch offset**: rotate the output in the compositor (for
  example `wlr-randr --transform 90`) and map the touch device to it. LiteBrowser
  re-lays out whenever its window is resized or rotated.
- **Text too small on a high-DPI panel**: use `--zoom 1.5`, or `GDK_SCALE=2` to scale
  the toolbar and keyboard too.
- **Blank or flickering pages** on some ARM GPUs: try `--no-gpu`.
- **A system on-screen keyboard is already running** (GNOME, Phosh's squeekboard): use
  `--keyboard off`, or `--keyboard touch` if a physical keyboard is attached too.

### Keyboard shortcuts (with a physical keyboard)

| Keys | Action |
|---|---|
| Ctrl+L / F6 | Focus the address bar |
| Ctrl+R / F5 | Reload |
| Esc (in the address bar) | Stop loading |
| Alt+← / Alt+→ | Back / forward |
| Ctrl `+` / `-` / `0` | Zoom in / out / reset |
| F12 | Developer tools |

## How it works

```
┌─────────────────────────── tao window ─────────────────────────────┐
│ toolbar WebView    ui/toolbar.html                                  │──┐
├─────────────────────────────────────────────────────────────────────┤  │ window.ipc
│ page WebView       the web page + ui/inject.js (shortcuts, SPA      │──┤ .postMessage
│                    routes, text-field taps, typing helper)          │  │ ({"cmd": …})
├─────────────────────────────────────────────────────────────────────┤  │
│ keyboard WebView   ui/keyboard.html (shown when a field is tapped)  │──┤
└─────────────────────────────────────────────────────────────────────┘  ▼
                Rust event loop (src/main.rs): load_url / evaluate_script
```

- `src/main.rs`: window, the three WebViews inside a `GtkFixed`, and the event loop
- `src/ipc.rs`: the JSON command protocol, and **which WebView may send what**. Pages
  can trigger shortcuts and report focused text fields, but can't navigate the browser or
  type. Only the keyboard WebView can type. A route-change notice carries no data: Rust
  reads the real URL itself, so a page can't spoof the address bar.
- `src/layout.rs`: splits the screen between toolbar, page and keyboard
- `src/options.rs`: command-line parsing
- `src/address.rs`: address-bar input → URL
- `src/serve.rs`: the localhost static server for local folders and files (GET/HEAD
  only, cannot escape the served folder)
- `ui/error.html`: the retrying error page

Profile data lives in `$XDG_DATA_HOME/litebrowser` (default `~/.local/share/litebrowser`).

## Development

```sh
cargo test                                   # unit tests
cargo clippy --all-targets -- -D warnings
cargo fmt --check
LITEBROWSER_DEBUG=1 cargo run -- URL         # log every message between the WebViews
```

## Limitations

- One page per window, no tabs.
- The on-screen keyboard is US-English QWERTY.
- Pages opened as `file://` (with `--no-serve`, or typed into the address bar) can't
  send messages to the browser. That turns off the on-screen keyboard, shortcuts and
  route syncing for those pages. This is a limitation of the wry library. Serving the
  folder (the default) avoids it.
- Text fields inside iframes don't get the on-screen keyboard.
