//! LiteBrowser — a small web browser that delegates rendering to the
//! operating system's WebView (WebKitGTK on Linux, WebView2 on Windows,
//! WKWebView on macOS). That gives full HTML5 / modern JavaScript support,
//! so React, Vue, etc. single-page apps work, in a binary of a few MB.
//!
//! The window holds two WebViews: a thin toolbar (`ui/toolbar.html`) and the
//! page itself. They talk to Rust over `window.ipc.postMessage`, and Rust
//! routes everything through the tao event loop as a `UserEvent`.

mod address;
mod ipc;
mod serve;

use std::path::PathBuf;

use ipc::Command;
use tao::{
    dpi::{LogicalPosition, LogicalSize},
    event::{Event, WindowEvent},
    event_loop::{ControlFlow, EventLoopBuilder, EventLoopProxy},
    window::{Window, WindowBuilder},
};
use wry::{NewWindowResponse, PageLoadEvent, Rect, WebContext, WebView, WebViewBuilder};

const APP_NAME: &str = "LiteBrowser";
const TOOLBAR_HEIGHT: u32 = 44;
const TOOLBAR_HTML: &str = include_str!("../ui/toolbar.html");
const START_HTML: &str = include_str!("../ui/start.html");
const SHORTCUTS_JS: &str = include_str!("../ui/shortcuts.js");
const ZOOM_STEPS: &[f64] = &[0.5, 0.67, 0.75, 0.8, 0.9, 1.0, 1.1, 1.25, 1.5, 1.75, 2.0, 2.5, 3.0];

const USAGE: &str = "\
Usage: litebrowser [OPTIONS] [URL | FILE | FOLDER]

A local FOLDER (e.g. a React build's dist/) or FILE is served on a private
127.0.0.1 port, with unknown routes falling back to index.html.

Options:
      --home <URL>   Page opened by the Home button (default: built-in start page)
      --incognito    Do not persist cookies, storage or cache
      --devtools     Open the developer tools on startup
      --no-serve     Open local files as file:// URLs instead of serving them
  -h, --help         Print this help";

#[derive(Debug)]
enum UserEvent {
    /// A command, and whether it came from the toolbar (trusted) or the page.
    Command(Command, bool),
    /// Page load started (`loading: true`) or finished.
    PageLoad {
        loading: bool,
        url: String,
    },
    TitleChanged(String),
    /// `window.open` / `target="_blank"`: opened in the current view.
    OpenInPlace(String),
}

#[derive(Debug, Default)]
struct Options {
    url: Option<String>,
    home: Option<String>,
    incognito: bool,
    devtools: bool,
    no_serve: bool,
    /// A local file or folder given on the command line.
    local: Option<PathBuf>,
}

fn parse_args() -> Options {
    let mut opts = Options::default();
    let mut args = std::env::args().skip(1);
    while let Some(arg) = args.next() {
        match arg.as_str() {
            "-h" | "--help" => {
                println!("{USAGE}");
                std::process::exit(0);
            }
            "--incognito" => opts.incognito = true,
            "--devtools" => opts.devtools = true,
            "--no-serve" => opts.no_serve = true,
            "--home" => opts.home = args.next().map(|h| address::resolve(&h)),
            a if a.starts_with('-') => {
                eprintln!("unknown option: {a}\n\n{USAGE}");
                std::process::exit(2);
            }
            _ => match PathBuf::from(&arg).canonicalize() {
                Ok(path) if !arg.contains("://") => opts.local = Some(path),
                _ => opts.url = Some(address::resolve(&arg)),
            },
        }
    }
    if let Some(path) = opts.local.take() {
        opts.url = Some(open_local(&path, opts.no_serve));
    }
    opts
}

/// URL for a local file or folder: served over http:// by default (see
/// `serve`), or a plain file:// URL with `--no-serve`.
fn open_local(path: &std::path::Path, no_serve: bool) -> String {
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

/// Where cookies, localStorage, IndexedDB and the HTTP cache are kept.
/// (Ignored by WKWebView on macOS, which manages its own store.)
fn data_dir() -> Option<PathBuf> {
    let base = if cfg!(windows) {
        std::env::var_os("LOCALAPPDATA").map(PathBuf::from)
    } else if cfg!(target_os = "macos") {
        std::env::var_os("HOME").map(|h| PathBuf::from(h).join("Library/Application Support"))
    } else {
        std::env::var_os("XDG_DATA_HOME")
            .map(PathBuf::from)
            .or_else(|| std::env::var_os("HOME").map(|h| PathBuf::from(h).join(".local/share")))
    };
    base.map(|b| b.join("litebrowser"))
}

fn layout(window: &Window) -> (Rect, Rect) {
    let size = window.inner_size().to_logical::<u32>(window.scale_factor());
    let toolbar_h = TOOLBAR_HEIGHT.min(size.height);
    let toolbar = Rect {
        position: LogicalPosition::new(0, 0).into(),
        size: LogicalSize::new(size.width, toolbar_h).into(),
    };
    let content = Rect {
        position: LogicalPosition::new(0, toolbar_h).into(),
        size: LogicalSize::new(size.width, size.height - toolbar_h).into(),
    };
    (toolbar, content)
}

/// Sends `ui.<method>(<json arg>)` to the toolbar.
fn toolbar_call(toolbar: &WebView, method: &str, arg: impl serde::Serialize) {
    if let Ok(json) = serde_json::to_string(&arg) {
        let _ = toolbar.evaluate_script(&format!("window.ui && ui.{method}({json})"));
    }
}

fn ipc_handler(proxy: EventLoopProxy<UserEvent>, from_toolbar: bool) -> impl Fn(wry::http::Request<String>) {
    move |req| match Command::parse(req.body()) {
        Some(cmd) if from_toolbar || cmd.allowed_from_page() => {
            let _ = proxy.send_event(UserEvent::Command(cmd, from_toolbar));
        }
        _ => {}
    }
}

fn main() -> wry::Result<()> {
    let opts = parse_args();
    let event_loop = EventLoopBuilder::<UserEvent>::with_user_event().build();
    let proxy = event_loop.create_proxy();

    let window = WindowBuilder::new()
        .with_title(APP_NAME)
        .with_inner_size(LogicalSize::new(1200, 800))
        .with_min_inner_size(LogicalSize::new(400, 200))
        .build(&event_loop)
        .expect("failed to create window");

    // On Linux the WebViews live in a GtkFixed so they can be positioned.
    #[cfg(any(
        target_os = "linux",
        target_os = "dragonfly",
        target_os = "freebsd",
        target_os = "netbsd",
        target_os = "openbsd"
    ))]
    let fixed = {
        use gtk::prelude::*;
        use tao::platform::unix::WindowExtUnix;
        let fixed = gtk::Fixed::new();
        let vbox = window.default_vbox().expect("tao window has no default vbox");
        vbox.pack_start(&fixed, true, true, 0);
        fixed.show_all();
        fixed
    };
    let build = |builder: WebViewBuilder<'_>| -> wry::Result<WebView> {
        #[cfg(any(
            target_os = "linux",
            target_os = "dragonfly",
            target_os = "freebsd",
            target_os = "netbsd",
            target_os = "openbsd"
        ))]
        {
            use wry::WebViewBuilderExtUnix;
            builder.build_gtk(&fixed)
        }
        #[cfg(not(any(
            target_os = "linux",
            target_os = "dragonfly",
            target_os = "freebsd",
            target_os = "netbsd",
            target_os = "openbsd"
        )))]
        {
            builder.build_as_child(&window)
        }
    };

    let (toolbar_bounds, content_bounds) = layout(&window);

    let toolbar = build(
        WebViewBuilder::new()
            .with_bounds(toolbar_bounds)
            .with_html(TOOLBAR_HTML)
            .with_initialization_script(format!("window.__liteToolbar = true;\n{SHORTCUTS_JS}"))
            .with_ipc_handler(ipc_handler(proxy.clone(), true))
            .with_hotkeys_zoom(false),
    )?;

    let mut web_context = WebContext::new(if opts.incognito { None } else { data_dir() });
    let content = {
        let (p_load, p_title, p_new) = (proxy.clone(), proxy.clone(), proxy.clone());
        let mut builder = WebViewBuilder::new_with_web_context(&mut web_context)
            .with_bounds(content_bounds)
            .with_initialization_script_for_main_only(SHORTCUTS_JS, true)
            .with_ipc_handler(ipc_handler(proxy.clone(), false))
            .with_devtools(true)
            .with_clipboard(true)
            .with_autoplay(true)
            .with_hotkeys_zoom(false)
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
        builder = match &opts.url {
            Some(url) => builder.with_url(url),
            None => match &opts.home {
                Some(home) => builder.with_url(home),
                None => builder.with_html(START_HTML),
            },
        };
        build(builder)?
    };

    if opts.devtools {
        content.open_devtools();
    }
    if opts.url.is_none() && opts.home.is_none() {
        toolbar_call(&toolbar, "focusAddress", ());
    }

    let mut zoom = 1.0_f64;

    event_loop.run(move |event, _, control_flow| {
        *control_flow = ControlFlow::Wait;
        // Keep the profile's WebContext alive as long as the WebViews.
        let _ = &web_context;

        match event {
            Event::WindowEvent {
                event: WindowEvent::CloseRequested,
                ..
            } => {
                *control_flow = ControlFlow::Exit;
            }
            Event::WindowEvent {
                event: WindowEvent::Resized(_) | WindowEvent::ScaleFactorChanged { .. },
                ..
            } => {
                let (tb, ct) = layout(&window);
                let _ = toolbar.set_bounds(tb);
                let _ = content.set_bounds(ct);
            }
            Event::UserEvent(UserEvent::PageLoad { loading, url }) => {
                toolbar_call(&toolbar, "setLoading", loading);
                toolbar_call(&toolbar, "setUrl", &url);
                if !loading && zoom != 1.0 {
                    let _ = content.zoom(zoom);
                }
            }
            Event::UserEvent(UserEvent::TitleChanged(title)) => {
                let title = title.trim();
                window.set_title(&if title.is_empty() {
                    APP_NAME.to_string()
                } else {
                    format!("{title} — {APP_NAME}")
                });
            }
            Event::UserEvent(UserEvent::OpenInPlace(url)) => {
                let _ = content.load_url(&url);
            }
            Event::UserEvent(UserEvent::Command(cmd, from_toolbar)) => {
                debug_assert!(from_toolbar || cmd.allowed_from_page());
                match cmd {
                    Command::Navigate { input } => {
                        let url = address::resolve(&input);
                        toolbar_call(&toolbar, "setUrl", &url);
                        let _ = content.load_url(&url);
                        let _ = content.focus();
                    }
                    Command::Home => {
                        let _ = match &opts.home {
                            Some(home) => content.load_url(home),
                            None => content.load_html(START_HTML),
                        };
                    }
                    Command::Back => {
                        let _ = content.evaluate_script("history.back()");
                    }
                    Command::Forward => {
                        let _ = content.evaluate_script("history.forward()");
                    }
                    Command::Reload => {
                        let _ = content.reload();
                    }
                    Command::Stop => {
                        let _ = content.evaluate_script("window.stop()");
                        toolbar_call(&toolbar, "setLoading", false);
                    }
                    Command::FocusAddress => {
                        let _ = toolbar.focus();
                        toolbar_call(&toolbar, "focusAddress", ());
                    }
                    Command::ToggleDevtools => {
                        if content.is_devtools_open() {
                            content.close_devtools();
                        } else {
                            content.open_devtools();
                        }
                    }
                    Command::UrlChanged => {
                        if let Ok(url) = content.url() {
                            toolbar_call(&toolbar, "setUrl", &url);
                        }
                    }
                    Command::ZoomIn | Command::ZoomOut | Command::ZoomReset => {
                        zoom = step_zoom(zoom, &cmd);
                        let _ = content.zoom(zoom);
                        toolbar_call(&toolbar, "setZoom", (zoom * 100.0).round() as u32);
                    }
                }
            }
            _ => {}
        }
    });
}

fn step_zoom(current: f64, cmd: &Command) -> f64 {
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
        _ => 1.0,
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn zoom_steps_clamp_at_the_ends() {
        assert_eq!(step_zoom(1.0, &Command::ZoomIn), 1.1);
        assert_eq!(step_zoom(1.0, &Command::ZoomOut), 0.9);
        assert_eq!(step_zoom(3.0, &Command::ZoomIn), 3.0);
        assert_eq!(step_zoom(0.5, &Command::ZoomOut), 0.5);
        assert_eq!(step_zoom(2.5, &Command::ZoomReset), 1.0);
    }
}
