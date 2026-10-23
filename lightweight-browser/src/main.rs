//! LiteBrowser — a small touch-screen web browser for Linux.
//!
//! Rendering is delegated to WebKitGTK (the engine behind GNOME Web), which
//! gives full HTML5 / modern JavaScript support, so React, Vue, etc.
//! single-page apps work, in a binary of about 1 MB.
//!
//! The window stacks up to three WebViews: a finger-sized toolbar
//! (`ui/toolbar.html`), the page, and an on-screen keyboard
//! (`ui/keyboard.html`) that slides in when a text field is tapped. They talk
//! to Rust over `window.ipc.postMessage`, and Rust routes everything through
//! the tao event loop as a `UserEvent`.

#[cfg(not(target_os = "linux"))]
compile_error!("LiteBrowser targets Linux (WebKitGTK).");

mod address;
mod ipc;
mod layout;
mod options;
mod serve;

use std::path::{Path, PathBuf};

use gtk::prelude::*;
use ipc::{Command, Source};
use options::{KeyboardMode, Options, Parsed};
use tao::{
    dpi::{LogicalPosition, LogicalSize, PhysicalPosition, Size},
    event::{Event, WindowEvent},
    event_loop::{ControlFlow, EventLoopBuilder, EventLoopProxy},
    platform::unix::WindowExtUnix,
    window::{Fullscreen, Window, WindowBuilder},
};
use webkit2gtk::{SettingsExt, UserContentManagerExt, WebViewExt};
use wry::{
    NewWindowResponse, PageLoadEvent, Rect, WebContext, WebView, WebViewBuilder, WebViewBuilderExtUnix, WebViewExtUnix,
};

const APP_NAME: &str = "LiteBrowser";
const TOOLBAR_HTML: &str = include_str!("../ui/toolbar.html");
const KEYBOARD_HTML: &str = include_str!("../ui/keyboard.html");
const START_HTML: &str = include_str!("../ui/start.html");
const INJECT_JS: &str = include_str!("../ui/inject.js");
const ERROR_HTML: &str = include_str!("../ui/error.html");
const ZOOM_STEPS: &[f64] = &[0.5, 0.67, 0.75, 0.8, 0.9, 1.0, 1.1, 1.25, 1.5, 1.75, 2.0, 2.5, 3.0];

#[derive(Debug)]
enum UserEvent {
    Command(Command, Source),
    /// Page load started (`loading: true`) or finished.
    PageLoad {
        loading: bool,
        url: String,
    },
    TitleChanged(String),
    /// `window.open` / `target="_blank"`: opened in the current view.
    OpenInPlace(String),
}

fn main() -> wry::Result<()> {
    let mut opts = match options::parse(std::env::args().skip(1)) {
        Ok(Parsed::Run(opts)) => opts,
        Ok(Parsed::Help) => {
            println!("{}", options::USAGE);
            return Ok(());
        }
        Err(err) => {
            eprintln!("{err}\n\n{}", options::USAGE);
            std::process::exit(2);
        }
    };
    if let Some(path) = opts.local.take() {
        opts.url = Some(open_local(&path, opts.no_serve));
    }
    if !opts.gpu {
        // Must be set before WebKit starts its web process.
        std::env::set_var("WEBKIT_DISABLE_COMPOSITING_MODE", "1");
        std::env::set_var("WEBKIT_DISABLE_DMABUF_RENDERER", "1");
    }

    let event_loop = EventLoopBuilder::<UserEvent>::with_user_event().build();
    let proxy = event_loop.create_proxy();

    // In fullscreen mode, also cover the monitor explicitly: a bare X server
    // started with xinit (common on kiosks) has no window manager to honour
    // the fullscreen request.
    let monitor = event_loop
        .primary_monitor()
        .or_else(|| event_loop.available_monitors().next());
    let (position, size): (PhysicalPosition<i32>, Size) = match (&monitor, opts.fullscreen) {
        (Some(m), true) => (m.position(), m.size().into()),
        _ => (PhysicalPosition::new(0, 0), LogicalSize::new(1024, 600).into()),
    };
    let window = WindowBuilder::new()
        .with_title(APP_NAME)
        .with_inner_size(size)
        .with_position(position)
        .with_min_inner_size(LogicalSize::new(320, 240))
        .with_fullscreen(opts.fullscreen.then_some(Fullscreen::Borderless(None)))
        .with_decorations(!opts.fullscreen)
        .build(&event_loop)
        .expect("failed to create window");
    if opts.hide_cursor {
        window.set_cursor_visible(false);
    }

    // The WebViews live in a GtkFixed so they can be positioned and resized.
    let fixed = gtk::Fixed::new();
    let vbox = window.default_vbox().expect("tao window has no default vbox");
    vbox.pack_start(&fixed, true, true, 0);
    fixed.show_all();

    let (width, height) = logical_size(&window);
    let areas = layout::compute(height, opts.toolbar, false);
    let prelude = |toolbar: bool| {
        format!(
            "window.__liteToolbar = {toolbar};\nwindow.__liteKeyboard = {:?};\n{INJECT_JS}",
            opts.keyboard.as_js()
        )
    };

    let keyboard = if opts.keyboard != KeyboardMode::Off {
        let webview = WebViewBuilder::new()
            .with_bounds(rect(
                width,
                layout::Band {
                    y: height.saturating_sub(1),
                    height: 1,
                },
            ))
            .with_html(KEYBOARD_HTML)
            .with_ipc_handler(ipc_handler(proxy.clone(), Source::Keyboard))
            .with_hotkeys_zoom(false)
            .build_gtk(&fixed)?;
        // Created first so it sits *under* the page, and shown by resizing
        // rather than toggling GTK visibility: mapping a WebView makes
        // WebKitGTK blur the field being typed into.
        // Tapping a key must not steal focus from that field either.
        let widget = webview.webview();
        widget.set_can_focus(false);
        widget.set_focus_on_click(false);
        Some(webview)
    } else {
        None
    };

    let toolbar = if opts.toolbar {
        let webview = WebViewBuilder::new()
            .with_bounds(rect(width, areas.toolbar))
            .with_html(TOOLBAR_HTML)
            .with_initialization_script(prelude(true))
            .with_ipc_handler(ipc_handler(proxy.clone(), Source::Toolbar))
            .with_hotkeys_zoom(false)
            .build_gtk(&fixed)?;
        Some(webview)
    } else {
        None
    };

    let mut web_context = WebContext::new(if opts.incognito { None } else { data_dir() });
    let content = {
        let (p_load, p_title, p_new) = (proxy.clone(), proxy.clone(), proxy.clone());
        let mut builder = WebViewBuilder::new_with_web_context(&mut web_context)
            .with_bounds(rect(width, areas.content))
            .with_initialization_script_for_main_only(prelude(false), true)
            .with_ipc_handler(ipc_handler(proxy.clone(), Source::Page))
            .with_devtools(!opts.kiosk)
            .with_clipboard(true)
            .with_autoplay(true)
            .with_hotkeys_zoom(false)
            // Horizontal swipe from the screen edge goes back / forward.
            .with_back_forward_navigation_gestures(true)
            .with_incognito(opts.incognito)
            .with_focused(true)
            .with_on_page_load_handler(move |ev, url| {
                let loading = matches!(ev, PageLoadEvent::Started);
                let _ = p_load.send_event(UserEvent::PageLoad { loading, url });
            })
            .with_document_title_changed_handler(move |title| {
                let _ = p_title.send_event(UserEvent::TitleChanged(title));
            })
            .with_new_window_req_handler(move |url, _features| {
                let _ = p_new.send_event(UserEvent::OpenInPlace(url));
                NewWindowResponse::Deny
            });
        if opts.kiosk {
            builder = builder
                .with_initialization_script("window.addEventListener('contextmenu', (e) => e.preventDefault(), true);");
        }
        builder = match (&opts.url, &opts.home) {
            (Some(url), _) | (None, Some(url)) => builder.with_url(url),
            (None, None) => builder.with_html(START_HTML),
        };
        builder.build_gtk(&fixed)?
    };

    // Replace WebKit's blank failed-load view with a retrying error page.
    content.webview().connect_load_failed(|webview, _event, uri, err| {
        if err.matches(webkit2gtk::NetworkError::Cancelled)
            || err.matches(webkit2gtk::PolicyError::FrameLoadInterruptedByPolicyChange)
        {
            return false;
        }
        let html = ERROR_HTML
            .replace("{{URL}}", &html_escape(uri))
            .replace("{{MESSAGE}}", &html_escape(err.message()));
        webview.load_alternate_html(&html, uri, None);
        true
    });

    let webviews = [toolbar.as_ref(), Some(&content), keyboard.as_ref()];
    for webview in webviews.into_iter().flatten() {
        let widget = webview.webview();
        if let Some(settings) = WebViewExt::settings(&widget) {
            settings.set_enable_smooth_scrolling(true);
            if !opts.gpu {
                settings.set_hardware_acceleration_policy(webkit2gtk::HardwareAccelerationPolicy::Never);
            }
        }
        if opts.hide_cursor {
            if let Some(manager) = widget.user_content_manager() {
                manager.add_style_sheet(&webkit2gtk::UserStyleSheet::new(
                    "*, *::before, *::after { cursor: none !important; }",
                    webkit2gtk::UserContentInjectedFrames::AllFrames,
                    webkit2gtk::UserStyleLevel::User,
                    &[],
                    &[],
                ));
            }
        }
    }

    if opts.devtools {
        content.open_devtools();
    }
    if opts.zoom != 1.0 {
        let _ = content.zoom(opts.zoom);
    }

    let mut browser = Browser {
        fixed,
        zoom: opts.zoom,
        opts,
        window,
        toolbar,
        content,
        keyboard,
        keyboard_for: None,
    };
    browser.toolbar_call("setZoom", (browser.zoom * 100.0).round() as u32);
    browser.relayout();

    event_loop.run(move |event, _, control_flow| {
        *control_flow = ControlFlow::Wait;
        // Keep the profile's WebContext alive as long as the WebViews.
        let _ = &web_context;

        match event {
            Event::WindowEvent {
                event: WindowEvent::CloseRequested,
                ..
            } => *control_flow = ControlFlow::Exit,
            Event::WindowEvent {
                event: WindowEvent::Resized(_) | WindowEvent::ScaleFactorChanged { .. },
                ..
            } => browser.relayout(),
            Event::UserEvent(UserEvent::PageLoad { loading, url }) => {
                browser.toolbar_call("setLoading", loading);
                browser.toolbar_call("setUrl", &url);
                if loading && browser.keyboard_for == Some(Source::Page) {
                    browser.set_keyboard(None);
                }
                if !loading && browser.zoom != 1.0 {
                    let _ = browser.content.zoom(browser.zoom);
                }
            }
            Event::UserEvent(UserEvent::TitleChanged(title)) => {
                let title = title.trim();
                browser.window.set_title(&if title.is_empty() {
                    APP_NAME.to_string()
                } else {
                    format!("{title} — {APP_NAME}")
                });
            }
            Event::UserEvent(UserEvent::OpenInPlace(url)) => {
                let _ = browser.content.load_url(&url);
            }
            Event::UserEvent(UserEvent::Command(cmd, source)) => browser.handle(cmd, source),
            _ => {}
        }
    });
}

struct Browser {
    opts: Options,
    /// GTK container holding the WebViews, stacked bottom to top: keyboard,
    /// toolbar, page.
    fixed: gtk::Fixed,
    window: Window,
    toolbar: Option<WebView>,
    content: WebView,
    keyboard: Option<WebView>,
    /// Which WebView the on-screen keyboard is typing into, while it is shown.
    keyboard_for: Option<Source>,
    zoom: f64,
}

impl Browser {
    fn handle(&mut self, cmd: Command, source: Source) {
        debug_assert!(cmd.allowed_from(source));
        match cmd {
            Command::Navigate { input } => {
                let url = address::resolve(&input);
                self.toolbar_call("setUrl", &url);
                let _ = self.content.load_url(&url);
                let _ = self.content.focus();
            }
            Command::Home => {
                let _ = match &self.opts.home {
                    Some(home) => self.content.load_url(home),
                    None => self.content.load_html(START_HTML),
                };
            }
            Command::Back => {
                let _ = self.content.evaluate_script("history.back()");
            }
            Command::Forward => {
                let _ = self.content.evaluate_script("history.forward()");
            }
            Command::Reload => {
                let _ = self.content.reload();
            }
            Command::Stop => {
                let _ = self.content.evaluate_script("window.stop()");
                self.toolbar_call("setLoading", false);
            }
            Command::FocusAddress => {
                if let Some(toolbar) = &self.toolbar {
                    let _ = toolbar.focus();
                }
                self.toolbar_call("focusAddress", ());
            }
            Command::ToggleDevtools if !self.opts.kiosk => {
                if self.content.is_devtools_open() {
                    self.content.close_devtools();
                } else {
                    self.content.open_devtools();
                }
            }
            Command::ToggleDevtools => {}
            Command::ZoomIn | Command::ZoomOut | Command::ZoomReset => {
                self.zoom = step_zoom(self.zoom, &cmd, self.opts.zoom);
                let _ = self.content.zoom(self.zoom);
                self.toolbar_call("setZoom", (self.zoom * 100.0).round() as u32);
            }
            Command::UrlChanged => {
                if let Ok(url) = self.content.url() {
                    self.toolbar_call("setUrl", &url);
                }
            }
            Command::Keyboard { show: true } => self.set_keyboard(Some(source)),
            Command::Keyboard { show: false } => {
                if self.keyboard_for == Some(source) {
                    self.set_keyboard(None);
                }
            }
            Command::HideKeyboard => {
                // Blur the field too, so tapping it again brings the keyboard back.
                if let Some(target) = self.keyboard_target() {
                    let _ = target.evaluate_script("document.activeElement && document.activeElement.blur()");
                }
                self.set_keyboard(None);
            }
            Command::Type { text } => self.send_key(serde_json::json!({ "text": text })),
            Command::Key { key } => self.send_key(serde_json::json!({ "key": key })),
        }
    }

    fn keyboard_target(&self) -> Option<&WebView> {
        match self.keyboard_for? {
            Source::Toolbar => self.toolbar.as_ref(),
            Source::Page => Some(&self.content),
            Source::Keyboard => None,
        }
    }

    fn send_key(&self, key: serde_json::Value) {
        if let Some(target) = self.keyboard_target() {
            let _ = target.evaluate_script(&format!("window.__liteKey && __liteKey({key})"));
        }
    }

    fn set_keyboard(&mut self, target: Option<Source>) {
        let Some(keyboard) = &self.keyboard else { return };
        let was_shown = self.keyboard_for.is_some();
        self.keyboard_for = target;
        if was_shown != target.is_some() {
            if target.is_some() {
                let _ = keyboard.evaluate_script("window.kbReset && kbReset()");
            }
            self.relayout();
        }
        if let Some(target) = self.keyboard_target() {
            let _ = target.evaluate_script("window.__liteKeyboardShown && __liteKeyboardShown()");
        }
    }

    fn relayout(&self) {
        let (width, height) = logical_size(&self.window);
        let areas = layout::compute(height, self.toolbar.is_some(), self.keyboard_for.is_some());
        if let Some(toolbar) = &self.toolbar {
            self.place(toolbar, width, areas.toolbar);
        }
        self.place(&self.content, width, areas.content);
        if let Some(keyboard) = &self.keyboard {
            // While hidden it shrinks to a 1 px strip underneath the page, so
            // it can't show through a page that hasn't painted yet.
            let band = if self.keyboard_for.is_some() {
                areas.keyboard
            } else {
                layout::Band {
                    y: height.saturating_sub(1),
                    height: 1,
                }
            };
            self.place(keyboard, width, band);
        }
    }

    /// Positions a WebView inside the GtkFixed. (wry's `set_bounds` only
    /// reallocates the widget, which GTK undoes on its next layout pass.)
    fn place(&self, webview: &WebView, width: u32, band: layout::Band) {
        let widget = webview.webview();
        self.fixed.move_(&widget, 0, band.y as i32);
        widget.set_size_request(width as i32, band.height as i32);
        let _ = webview.set_bounds(rect(width, band));
    }

    /// Calls `ui.<method>(<json arg>)` in the toolbar, if there is one.
    fn toolbar_call(&self, method: &str, arg: impl serde::Serialize) {
        if let (Some(toolbar), Ok(json)) = (&self.toolbar, serde_json::to_string(&arg)) {
            let _ = toolbar.evaluate_script(&format!("window.ui && ui.{method}({json})"));
        }
    }
}

/// Set `LITEBROWSER_DEBUG=1` to log every message the WebViews send.
fn ipc_handler(proxy: EventLoopProxy<UserEvent>, source: Source) -> impl Fn(wry::http::Request<String>) {
    let debug = std::env::var_os("LITEBROWSER_DEBUG").is_some();
    move |req| {
        let cmd = Command::parse(req.body()).filter(|cmd| cmd.allowed_from(source));
        if debug {
            let verdict = if cmd.is_some() { "ok" } else { "rejected" };
            eprintln!("ipc {source:?} {verdict}: {}", req.body());
        }
        if let Some(cmd) = cmd {
            let _ = proxy.send_event(UserEvent::Command(cmd, source));
        }
    }
}

fn logical_size(window: &Window) -> (u32, u32) {
    let size = window.inner_size().to_logical::<u32>(window.scale_factor());
    (size.width, size.height)
}

fn rect(width: u32, band: layout::Band) -> Rect {
    Rect {
        position: LogicalPosition::new(0, band.y).into(),
        size: LogicalSize::new(width, band.height).into(),
    }
}

/// URL for a local file or folder: served over http:// by default (see
/// `serve`), or a plain file:// URL with `--no-serve`.
fn open_local(path: &Path, no_serve: bool) -> String {
    if no_serve {
        return address::resolve(&path.to_string_lossy());
    }
    let (root, file) = if path.is_dir() {
        (path.to_path_buf(), None)
    } else {
        (path.parent().unwrap_or(path).to_path_buf(), path.file_name())
    };
    match serve::start(root) {
        Ok(base) => {
            let file = file
                .map(|f| serve::encode_path(&f.to_string_lossy()))
                .unwrap_or_default();
            format!("{base}/{file}")
        }
        Err(err) => {
            eprintln!("could not start local server ({err}); falling back to file://");
            address::resolve(&path.to_string_lossy())
        }
    }
}

fn html_escape(s: &str) -> String {
    s.replace('&', "&amp;")
        .replace('<', "&lt;")
        .replace('>', "&gt;")
        .replace('"', "&quot;")
        .replace('\'', "&#39;")
}

/// Where cookies, localStorage, IndexedDB and the HTTP cache are kept.
fn data_dir() -> Option<PathBuf> {
    std::env::var_os("XDG_DATA_HOME")
        .map(PathBuf::from)
        .or_else(|| std::env::var_os("HOME").map(|h| PathBuf::from(h).join(".local/share")))
        .map(|base| base.join("litebrowser"))
}

/// Next zoom level; `ZoomReset` returns to the `--zoom` level.
fn step_zoom(current: f64, cmd: &Command, default: f64) -> f64 {
    match cmd {
        Command::ZoomIn => ZOOM_STEPS
            .iter()
            .copied()
            .find(|&z| z > current + 1e-6)
            .unwrap_or(current),
        Command::ZoomOut => ZOOM_STEPS
            .iter()
            .rev()
            .copied()
            .find(|&z| z < current - 1e-6)
            .unwrap_or(current),
        _ => default,
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn zoom_steps_clamp_at_the_ends() {
        assert_eq!(step_zoom(1.0, &Command::ZoomIn, 1.0), 1.1);
        assert_eq!(step_zoom(1.0, &Command::ZoomOut, 1.0), 0.9);
        assert_eq!(step_zoom(3.0, &Command::ZoomIn, 1.0), 3.0);
        assert_eq!(step_zoom(0.5, &Command::ZoomOut, 1.0), 0.5);
        assert_eq!(step_zoom(2.5, &Command::ZoomReset, 1.0), 1.0);
        assert_eq!(step_zoom(2.5, &Command::ZoomReset, 1.5), 1.5);
        assert_eq!(step_zoom(1.6, &Command::ZoomIn, 1.6), 1.75);
    }
}
