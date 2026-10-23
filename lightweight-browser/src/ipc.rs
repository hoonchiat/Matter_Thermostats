//! Messages posted from the WebViews to Rust via `window.ipc.postMessage(json)`.

use serde::Deserialize;

/// Browser actions. The toolbar may send any of them; web pages (through the
/// injected keyboard-shortcut script) only the ones `allowed_from_page` accepts.
#[derive(Debug, Clone, PartialEq, Deserialize)]
#[serde(tag = "cmd", rename_all = "kebab-case")]
pub enum Command {
    Navigate {
        input: String,
    },
    Back,
    Forward,
    Reload,
    Stop,
    Home,
    FocusAddress,
    ToggleDevtools,
    ZoomIn,
    ZoomOut,
    ZoomReset,
    /// The page changed its URL without a load (SPA routing: pushState,
    /// replaceState, hash changes). Rust re-reads the real URL itself, so a
    /// page cannot put arbitrary text in the address bar.
    UrlChanged,
}

impl Command {
    pub fn parse(body: &str) -> Option<Self> {
        serde_json::from_str(body).ok()
    }

    /// A page can trigger shortcuts, but must never steer the browser to a URL.
    pub fn allowed_from_page(&self) -> bool {
        !matches!(self, Command::Navigate { .. } | Command::Home)
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn parses_toolbar_messages() {
        assert_eq!(
            Command::parse(r#"{"cmd":"navigate","input":"react.dev"}"#),
            Some(Command::Navigate {
                input: "react.dev".into()
            })
        );
        assert_eq!(
            Command::parse(r#"{"cmd":"toggle-devtools"}"#),
            Some(Command::ToggleDevtools)
        );
        assert_eq!(Command::parse(r#"{"cmd":"zoom-in"}"#), Some(Command::ZoomIn));
        assert_eq!(Command::parse(r#"{"cmd":"url-changed"}"#), Some(Command::UrlChanged));
    }

    #[test]
    fn rejects_garbage() {
        assert_eq!(Command::parse("not json"), None);
        assert_eq!(Command::parse(r#"{"cmd":"rm-rf"}"#), None);
        assert_eq!(Command::parse(r#"{"cmd":"navigate"}"#), None);
    }

    #[test]
    fn pages_cannot_navigate() {
        assert!(!Command::Navigate {
            input: "evil.example".into()
        }
        .allowed_from_page());
        assert!(!Command::Home.allowed_from_page());
        assert!(Command::Reload.allowed_from_page());
        assert!(Command::FocusAddress.allowed_from_page());
    }
}
