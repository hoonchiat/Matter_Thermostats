# LiteBrowser

A lightweight desktop web browser with full **HTML5** support that runs modern
**React** (and Vue, Svelte, Angular…) single-page apps.

Rather than shipping its own engine (Chromium alone is >100 MB), LiteBrowser is a thin
Rust shell around the WebView that already comes with the operating system:

| OS | Engine | Notes |
|---|---|---|
| Linux | WebKitGTK 4.1 (WebKit, as in Safari) | install `libwebkit2gtk-4.1` |
| Windows 10/11 | Microsoft Edge WebView2 (Chromium) | preinstalled on Windows 11 |
| macOS | WKWebView (WebKit, as in Safari) | built in |

These engines handle everything a React app needs: ES2023+, ES modules and import maps,
`fetch`, Web Workers, WebSocket, WebGL, IndexedDB/localStorage, `<video>`/`<audio>`, CSS
grid/`:has()`, and the History API for client-side routing. The release binary is
**under 1 MB** on Linux.

## Features

- Toolbar: back, forward, reload/stop, home, address bar, developer tools
- Address bar takes URLs, bare domains (`react.dev` → `https://`), dev servers
  (`localhost:5173` → `http://`), absolute file paths, or search terms (DuckDuckGo)
- **Local React builds just work**: `litebrowser dist/` serves the folder on a private
  `127.0.0.1` port, so absolute asset paths (`/assets/index-abc.js`) resolve, ES modules
  aren't blocked by `file://` rules, and deep links like `/settings/profile` fall back to
  `index.html` (BrowserRouter-style routing)
- Address bar follows SPA route changes (`pushState`, `replaceState`, hash changes)
- Developer tools (F12) for inspecting React apps
- Zoom (Ctrl `+` / `-` / `0`)
- Persistent profile (cookies, storage, cache) or `--incognito`
- Pop-ups and `target="_blank"` links open in the current view
- Follows the system light/dark theme

## Build

Requires a Rust toolchain (1.80+).

```sh
# Linux only: the WebKitGTK development headers
sudo apt install libwebkit2gtk-4.1-dev libgtk-3-dev     # Debian/Ubuntu
sudo dnf install webkit2gtk4.1-devel gtk3-devel         # Fedora

cd lightweight-browser
cargo build --release
./target/release/litebrowser https://react.dev
```

## Usage

```
litebrowser [OPTIONS] [URL | FILE | FOLDER]

  --home <URL>   Page opened by the Home button (default: built-in start page)
  --incognito    Do not persist cookies, storage or cache
  --devtools     Open the developer tools on startup
  --no-serve     Open local files as file:// URLs instead of serving them
```

Examples:

```sh
litebrowser                         # start page
litebrowser react.dev               # a website
litebrowser localhost:5173          # your Vite / CRA / Next dev server
litebrowser my-app/dist             # a production React build, served locally
litebrowser examples/react-demo.html  # React 19 from a CDN + an HTML5 feature report
```

### Keyboard shortcuts

| Keys | Action |
|---|---|
| Ctrl+L / F6 | Focus the address bar |
| Ctrl+R / F5 | Reload |
| Esc (in the address bar) | Stop loading |
| Alt+← / Alt+→ | Back / forward |
| Ctrl `+` / `-` / `0` | Zoom in / out / reset |
| F12 | Developer tools |

On macOS use ⌘ instead of Ctrl.

## How it works

```
┌──────────────────────── tao window ────────────────────────┐
│ toolbar WebView  (ui/toolbar.html)                          │──┐ window.ipc.postMessage
├─────────────────────────────────────────────────────────────┤  │ ({"cmd": ...})
│ content WebView  (the web page)                             │──┤
│   + ui/shortcuts.js: keyboard shortcuts, SPA route changes  │  │
└─────────────────────────────────────────────────────────────┘  ▼
                         Rust event loop (src/main.rs) ── load_url / evaluate_script
```

- `src/main.rs` — window, the two WebViews, and the event loop that routes commands
- `src/ipc.rs` — the JSON command protocol. Web pages can only send shortcut commands
  (reload, back, zoom…), never a URL to navigate to. A route-change notice carries no
  data; Rust reads the real URL from the WebView, so a page can't spoof the address bar.
- `src/address.rs` — address-bar input → URL
- `src/serve.rs` — the localhost static server for local folders and files (GET/HEAD
  only, cannot escape the served folder)

Profile data lives in `~/.local/share/litebrowser` (Linux) or `%LOCALAPPDATA%\litebrowser`
(Windows). WKWebView on macOS manages its own store.

## Tests

```sh
cargo test                                   # unit tests
cargo clippy --all-targets -- -D warnings
```

## Limitations

- One page per window, no tabs yet.
- Rendering and web-platform support are whatever the OS engine provides. Sites that only
  test against Chrome may behave slightly differently on WebKit (Linux, macOS).
- Pages opened as `file://` (`--no-serve`, or typed into the address bar) can't send
  messages to the browser, so keyboard shortcuts and route syncing are off there. This is
  a limitation of the wry library. Toolbar buttons still work.
