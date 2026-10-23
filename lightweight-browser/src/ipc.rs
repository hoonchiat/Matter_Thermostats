//! Messages posted from the WebViews to Rust via `window.ipc.postMessage(json)`.

use serde::{Deserialize, Serialize};

/// Which WebView a message came from. Each one may only send some commands.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum Source {
    /// The browser toolbar (`ui/toolbar.html`).
    Toolbar,
    /// The web page, through the injected `ui/inject.js`.
    Page,
    /// The on-screen keyboard (`ui/keyboard.html`).
    Keyboard,
}

/// Non-character keys the on-screen keyboard can press.
#[derive(Debug, Clone, Copy, PartialEq, Eq, Deserialize, Serialize)]
pub enum SpecialKey {
    Backspace,
    Enter,
    ArrowLeft,
    ArrowRight,
}

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
    /// A text field gained (`true`) or lost (`false`) focus.
    Keyboard {
        show: bool,
    },
    /// On-screen keyboard: type characters into the focused field.
    Type {
        text: String,
    },
    /// On-screen keyboard: press a non-character key.
    Key {
        key: SpecialKey,
    },
    HideKeyboard,
}

/// Longest string one on-screen key can type (e.g. ".com").
const MAX_TYPED_CHARS: usize = 8;

impl Command {
    pub fn parse(body: &str) -> Option<Self> {
        serde_json::from_str(body).ok()
    }

    /// Pages can trigger shortcuts but never steer the browser to a URL or
    /// type into the toolbar; only the keyboard WebView can type.
    pub fn allowed_from(&self, source: Source) -> bool {
        match self {
            Command::Navigate { .. } | Command::Home => source == Source::Toolbar,
            Command::Type { text } => source == Source::Keyboard && text.chars().count() <= MAX_TYPED_CHARS,
            Command::Key { .. } => source == Source::Keyboard,
            Command::HideKeyboard => true,
            _ => source != Source::Keyboard,
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use Source::*;

    #[test]
    fn parses_messages() {
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
        assert_eq!(Command::parse(r#"{"cmd":"url-changed"}"#), Some(Command::UrlChanged));
        assert_eq!(
            Command::parse(r#"{"cmd":"keyboard","show":true}"#),
            Some(Command::Keyboard { show: true })
        );
        assert_eq!(
            Command::parse(r#"{"cmd":"type","text":"é"}"#),
            Some(Command::Type { text: "é".into() })
        );
        assert_eq!(
            Command::parse(r#"{"cmd":"key","key":"Backspace"}"#),
            Some(Command::Key {
                key: SpecialKey::Backspace
            })
        );
        assert_eq!(
            Command::parse(r#"{"cmd":"hide-keyboard"}"#),
            Some(Command::HideKeyboard)
        );
    }

    #[test]
    fn rejects_garbage() {
        assert_eq!(Command::parse("not json"), None);
        assert_eq!(Command::parse(r#"{"cmd":"rm-rf"}"#), None);
        assert_eq!(Command::parse(r#"{"cmd":"navigate"}"#), None);
        assert_eq!(Command::parse(r#"{"cmd":"key","key":"F13"}"#), None);
    }

    #[test]
    fn pages_cannot_navigate_or_type() {
        let nav = Command::Navigate {
            input: "evil.example".into(),
        };
        assert!(nav.allowed_from(Toolbar));
        assert!(!nav.allowed_from(Page));
        assert!(!nav.allowed_from(Keyboard));
        assert!(!Command::Home.allowed_from(Page));
        assert!(Command::Reload.allowed_from(Page));
        assert!(Command::Keyboard { show: true }.allowed_from(Page));

        let typed = Command::Type { text: "a".into() };
        assert!(typed.allowed_from(Keyboard));
        assert!(!typed.allowed_from(Page));
        assert!(!typed.allowed_from(Toolbar));
        assert!(!Command::Key { key: SpecialKey::Enter }.allowed_from(Page));
    }

    #[test]
    fn keyboard_can_only_type() {
        assert!(!Command::Reload.allowed_from(Keyboard));
        assert!(!Command::ToggleDevtools.allowed_from(Keyboard));
        assert!(Command::HideKeyboard.allowed_from(Keyboard));
        assert!(!Command::Type {
            text: "way too long text".into()
        }
        .allowed_from(Keyboard));
    }
}
