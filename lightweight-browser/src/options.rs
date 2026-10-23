//! Command-line options.

use std::path::PathBuf;

pub const USAGE: &str = "\
Usage: litebrowser [OPTIONS] [URL | FILE | FOLDER]

A local FOLDER (e.g. a React build's dist/) or FILE is served on a private
127.0.0.1 port, with unknown routes falling back to index.html.

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
  -h, --help               Print this help";

#[derive(Debug, Clone, Copy, PartialEq, Eq, Default)]
pub enum KeyboardMode {
    #[default]
    On,
    Touch,
    Off,
}

impl KeyboardMode {
    pub fn as_js(self) -> &'static str {
        match self {
            KeyboardMode::On => "on",
            KeyboardMode::Touch => "touch",
            KeyboardMode::Off => "off",
        }
    }
}

#[derive(Debug, Clone, PartialEq)]
pub struct Options {
    /// A URL, or (until `main` resolves it) nothing when `local` is set.
    pub url: Option<String>,
    /// A local file or folder given on the command line.
    pub local: Option<PathBuf>,
    pub home: Option<String>,
    pub incognito: bool,
    pub no_serve: bool,
    pub fullscreen: bool,
    pub toolbar: bool,
    pub hide_cursor: bool,
    pub kiosk: bool,
    pub zoom: f64,
    pub keyboard: KeyboardMode,
    pub gpu: bool,
    pub devtools: bool,
}

impl Default for Options {
    fn default() -> Self {
        Options {
            url: None,
            local: None,
            home: None,
            incognito: false,
            no_serve: false,
            fullscreen: false,
            toolbar: true,
            hide_cursor: false,
            kiosk: false,
            zoom: 1.0,
            keyboard: KeyboardMode::On,
            gpu: true,
            devtools: false,
        }
    }
}

/// What the command line asked for.
#[derive(Debug, PartialEq)]
pub enum Parsed {
    Run(Options),
    Help,
}

pub fn parse(args: impl IntoIterator<Item = String>) -> Result<Parsed, String> {
    let mut opts = Options::default();
    let mut args = args.into_iter();
    while let Some(arg) = args.next() {
        match arg.as_str() {
            "-h" | "--help" => return Ok(Parsed::Help),
            "--incognito" => opts.incognito = true,
            "--no-serve" => opts.no_serve = true,
            "--fullscreen" => opts.fullscreen = true,
            "--no-toolbar" => opts.toolbar = false,
            "--hide-cursor" => opts.hide_cursor = true,
            "--kiosk" => {
                opts.kiosk = true;
                opts.fullscreen = true;
                opts.toolbar = false;
                opts.hide_cursor = true;
            }
            "--no-gpu" => opts.gpu = false,
            "--devtools" => opts.devtools = true,
            "--home" => opts.home = Some(crate::address::resolve(&value("--home", &mut args)?)),
            "--zoom" => {
                let v = value("--zoom", &mut args)?;
                opts.zoom = v
                    .parse::<f64>()
                    .ok()
                    .filter(|z| (0.25..=5.0).contains(z))
                    .ok_or_else(|| format!("--zoom expects a number between 0.25 and 5, got {v:?}"))?;
            }
            "--keyboard" => {
                opts.keyboard = match value("--keyboard", &mut args)?.as_str() {
                    "on" => KeyboardMode::On,
                    "touch" => KeyboardMode::Touch,
                    "off" => KeyboardMode::Off,
                    other => return Err(format!("--keyboard expects on, touch or off, got {other:?}")),
                }
            }
            a if a.starts_with('-') => return Err(format!("unknown option: {a}")),
            _ => match PathBuf::from(&arg).canonicalize() {
                Ok(path) if !arg.contains("://") => opts.local = Some(path),
                _ => opts.url = Some(crate::address::resolve(&arg)),
            },
        }
    }
    if opts.kiosk && opts.devtools {
        return Err("--devtools cannot be used with --kiosk".into());
    }
    Ok(Parsed::Run(opts))
}

fn value(flag: &str, args: &mut impl Iterator<Item = String>) -> Result<String, String> {
    args.next().ok_or_else(|| format!("{flag} needs a value"))
}

#[cfg(test)]
mod tests {
    use super::*;

    fn run(args: &[&str]) -> Result<Options, String> {
        match parse(args.iter().map(|s| s.to_string()))? {
            Parsed::Run(o) => Ok(o),
            Parsed::Help => Err("help".into()),
        }
    }

    #[test]
    fn defaults_suit_a_touch_screen() {
        let o = run(&[]).unwrap();
        assert!(o.toolbar && o.gpu && !o.fullscreen);
        assert_eq!(o.keyboard, KeyboardMode::On);
        assert_eq!(o.zoom, 1.0);
    }

    #[test]
    fn kiosk_implies_fullscreen_without_chrome() {
        let o = run(&["--kiosk", "localhost:3000"]).unwrap();
        assert!(o.kiosk && o.fullscreen && o.hide_cursor && !o.toolbar);
        assert_eq!(o.url.as_deref(), Some("http://localhost:3000"));
    }

    #[test]
    fn values() {
        let o = run(&["--zoom", "1.5", "--keyboard", "touch", "--home", "react.dev"]).unwrap();
        assert_eq!(o.zoom, 1.5);
        assert_eq!(o.keyboard, KeyboardMode::Touch);
        assert_eq!(o.home.as_deref(), Some("https://react.dev"));
    }

    #[test]
    fn local_paths_are_detected() {
        let dir = std::env::temp_dir();
        let o = run(&[dir.to_str().unwrap()]).unwrap();
        assert_eq!(o.local, Some(dir.canonicalize().unwrap()));
        assert_eq!(o.url, None);
    }

    #[test]
    fn errors() {
        assert!(run(&["--zoom"]).is_err());
        assert!(run(&["--zoom", "abc"]).is_err());
        assert!(run(&["--zoom", "50"]).is_err());
        assert!(run(&["--keyboard", "maybe"]).is_err());
        assert!(run(&["--bogus"]).is_err());
        assert!(run(&["--kiosk", "--devtools"]).is_err());
        assert_eq!(run(&["-h"]), Err("help".into()));
    }
}
