//! Splits the window between the toolbar, the page and the on-screen keyboard.

/// Toolbar height in logical pixels: 48 px buttons plus padding, so every
/// control is a comfortable finger-sized target.
pub const TOOLBAR_HEIGHT: u32 = 60;
const KEYBOARD_MIN: u32 = 200;
const KEYBOARD_MAX: u32 = 340;
/// Never let the keyboard leave the page less than this much room.
const CONTENT_MIN: u32 = 120;

/// Where the toolbar goes. `Bottom` keeps the controls within reach on tall
/// portrait kiosks.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum Toolbar {
    Hidden,
    Top,
    Bottom,
}

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

/// Top to bottom: toolbar (`Top`), page, toolbar (`Bottom`), keyboard (if
/// shown). A bottom toolbar sits just above the keyboard, so the address bar
/// stays next to the keys while typing.
pub fn compute(height: u32, toolbar: Toolbar, keyboard: bool) -> Layout {
    let toolbar_h = if toolbar == Toolbar::Hidden {
        0
    } else {
        TOOLBAR_HEIGHT.min(height)
    };
    let rest = height - toolbar_h;
    let keyboard_h = if keyboard {
        (height * 2 / 5)
            .clamp(KEYBOARD_MIN, KEYBOARD_MAX)
            .min(rest.saturating_sub(CONTENT_MIN))
    } else {
        0
    };
    let content_h = rest - keyboard_h;
    let (toolbar_y, content_y) = match toolbar {
        Toolbar::Bottom => (content_h, 0),
        _ => (0, toolbar_h),
    };
    Layout {
        toolbar: Band {
            y: toolbar_y,
            height: toolbar_h,
        },
        content: Band {
            y: content_y,
            height: content_h,
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

    use Toolbar::*;

    /// The bands tile the window with no gaps or overlaps.
    fn covers_window(l: &Layout, height: u32) {
        let mut bands = [l.toolbar, l.content, l.keyboard];
        bands.sort_by_key(|b| (b.y, b.height));
        let mut y = 0;
        for b in bands.iter().filter(|b| b.height > 0) {
            assert_eq!(b.y, y, "{l:?}");
            y += b.height;
        }
        assert_eq!(y, height, "{l:?}");
    }

    #[test]
    fn small_800x480_panel() {
        let l = compute(480, Top, false);
        covers_window(&l, 480);
        assert_eq!(l.content.height, 420);

        let l = compute(480, Top, true);
        covers_window(&l, 480);
        assert_eq!(l.keyboard.height, 200);
        assert_eq!(l.content.height, 220);
    }

    #[test]
    fn large_1080p_panel_caps_keyboard() {
        let l = compute(1080, Top, true);
        covers_window(&l, 1080);
        assert_eq!(l.keyboard.height, KEYBOARD_MAX);
    }

    #[test]
    fn kiosk_without_toolbar() {
        let l = compute(600, Hidden, true);
        covers_window(&l, 600);
        assert_eq!(l.toolbar.height, 0);
        assert_eq!(l.content.y, 0);
        assert_eq!(l.keyboard.height, 240);
    }

    #[test]
    fn tiny_window_keeps_some_page_visible() {
        let l = compute(300, Top, true);
        covers_window(&l, 300);
        assert_eq!(l.content.height, CONTENT_MIN);
        let l = compute(30, Top, true);
        covers_window(&l, 30);
    }

    #[test]
    fn portrait_1080x1920() {
        let l = compute(1920, Top, false);
        covers_window(&l, 1920);
        assert_eq!(l.content.height, 1860);

        let l = compute(1920, Top, true);
        covers_window(&l, 1920);
        assert_eq!(l.keyboard.height, KEYBOARD_MAX);
        assert_eq!(l.content.height, 1920 - TOOLBAR_HEIGHT - KEYBOARD_MAX);
    }

    #[test]
    fn bottom_toolbar_sits_above_the_keyboard() {
        let l = compute(1920, Bottom, false);
        covers_window(&l, 1920);
        assert_eq!(l.content.y, 0);
        assert_eq!(l.toolbar.y, 1920 - TOOLBAR_HEIGHT);

        let l = compute(1920, Bottom, true);
        covers_window(&l, 1920);
        assert_eq!(l.content.y, 0);
        assert_eq!(l.toolbar.y + l.toolbar.height, l.keyboard.y);
        assert_eq!(l.keyboard.y + l.keyboard.height, 1920);
    }
}
