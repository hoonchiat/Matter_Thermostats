//! Splits the window between the toolbar, the page and the on-screen keyboard.

/// Toolbar height in logical pixels: 48 px buttons plus padding, so every
/// control is a comfortable finger-sized target.
pub const TOOLBAR_HEIGHT: u32 = 60;
const KEYBOARD_MIN: u32 = 200;
const KEYBOARD_MAX: u32 = 340;
/// Never let the keyboard leave the page less than this much room.
const CONTENT_MIN: u32 = 120;

/// A horizontal band of the window: `y` offset and `height`, full width.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct Band {
    pub y: u32,
    pub height: u32,
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct Layout {
    pub toolbar: Band,
    pub content: Band,
    pub keyboard: Band,
}

/// Top to bottom: toolbar (if shown), page, keyboard (if shown).
pub fn compute(height: u32, toolbar: bool, keyboard: bool) -> Layout {
    let toolbar_h = if toolbar { TOOLBAR_HEIGHT.min(height) } else { 0 };
    let rest = height - toolbar_h;
    let keyboard_h = if keyboard {
        (height * 2 / 5)
            .clamp(KEYBOARD_MIN, KEYBOARD_MAX)
            .min(rest.saturating_sub(CONTENT_MIN))
    } else {
        0
    };
    Layout {
        toolbar: Band {
            y: 0,
            height: toolbar_h,
        },
        content: Band {
            y: toolbar_h,
            height: rest - keyboard_h,
        },
        keyboard: Band {
            y: height - keyboard_h,
            height: keyboard_h,
        },
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn covers_window(l: &Layout, height: u32) {
        assert_eq!(l.toolbar.height + l.content.height + l.keyboard.height, height);
        assert_eq!(l.content.y, l.toolbar.height);
        assert_eq!(l.keyboard.y, l.content.y + l.content.height);
    }

    #[test]
    fn small_800x480_panel() {
        let l = compute(480, true, false);
        covers_window(&l, 480);
        assert_eq!(l.content.height, 420);

        let l = compute(480, true, true);
        covers_window(&l, 480);
        assert_eq!(l.keyboard.height, 200);
        assert_eq!(l.content.height, 220);
    }

    #[test]
    fn large_1080p_panel_caps_keyboard() {
        let l = compute(1080, true, true);
        covers_window(&l, 1080);
        assert_eq!(l.keyboard.height, KEYBOARD_MAX);
    }

    #[test]
    fn kiosk_without_toolbar() {
        let l = compute(600, false, true);
        covers_window(&l, 600);
        assert_eq!(l.toolbar.height, 0);
        assert_eq!(l.content.y, 0);
        assert_eq!(l.keyboard.height, 240);
    }

    #[test]
    fn tiny_window_keeps_some_page_visible() {
        let l = compute(300, true, true);
        covers_window(&l, 300);
        assert_eq!(l.content.height, CONTENT_MIN);
        let l = compute(30, true, true);
        covers_window(&l, 30);
    }
}
