#!/bin/sh
# Checks a device is ready to run LiteBrowser and prints what it found:
# CPU architecture, OS, WebKitGTK, the display session, screens and touch
# devices. Read-only; safe to run as a normal user.
#
#   sh tools/check-device.sh

ok()   { printf '  [ok]   %s\n' "$*"; }
warn() { printf '  [warn] %s\n' "$*"; }
fail() { printf '  [FAIL] %s\n' "$*"; }
have() { command -v "$1" >/dev/null 2>&1; }

echo "System"
ok "architecture: $(uname -m)"
if [ -r /etc/os-release ]; then
    . /etc/os-release
    ok "OS: ${PRETTY_NAME:-unknown}"
fi
if have ldd; then
    ok "glibc: $(ldd --version 2>/dev/null | head -n1 | awk '{print $NF}')"
fi
if [ -r /proc/device-tree/model ]; then
    ok "board: $(tr -d '\0' < /proc/device-tree/model)"
fi

echo
echo "WebKitGTK (the engine LiteBrowser uses)"
if ldconfig -p 2>/dev/null | grep -q 'libwebkit2gtk-4\.1\.so\.0'; then
    ok "runtime library libwebkit2gtk-4.1 is installed"
else
    fail "libwebkit2gtk-4.1 not found: sudo apt install libwebkit2gtk-4.1-0"
fi
if have pkg-config && pkg-config --exists webkit2gtk-4.1 2>/dev/null; then
    ok "development files present (version $(pkg-config --modversion webkit2gtk-4.1)): can build from source"
else
    warn "development files missing; only needed to build here: sudo apt install libwebkit2gtk-4.1-dev libgtk-3-dev"
fi
if have cargo; then
    ok "Rust: $(cargo --version)"
else
    warn "Rust not installed; only needed to build here: https://rustup.rs"
fi

echo
echo "Display session"
if [ -n "$WAYLAND_DISPLAY" ]; then
    ok "Wayland ($WAYLAND_DISPLAY)"
elif [ -n "$DISPLAY" ]; then
    ok "X11 ($DISPLAY)"
    if [ -n "$GDK_CORE_DEVICE_EVENTS" ]; then
        fail "GDK_CORE_DEVICE_EVENTS is set: touch will arrive as mouse clicks; unset it"
    fi
else
    warn "no graphical session in this shell (fine over SSH; run the browser from the device's session, or use the kiosk service)"
fi
if have wlr-randr && [ -n "$WAYLAND_DISPLAY" ]; then
    wlr-randr 2>/dev/null | grep -E '^[^ ]|current|Transform' | sed 's/^/         /'
elif have xrandr && [ -n "$DISPLAY" ]; then
    xrandr --query 2>/dev/null | grep -E ' connected' | sed 's/^/         /'
fi
for mode in /sys/class/drm/card*-*/modes; do
    [ -r "$mode" ] || continue
    status=$(cat "$(dirname "$mode")/status" 2>/dev/null)
    [ "$status" = connected ] || continue
    connector=$(basename "$(dirname "$mode")")
    native=$(head -n1 "$mode")
    ok "screen ${connector#card*-}: native mode $native"
    case "$native" in
        1920x1080) warn "  landscape panel: for portrait, rotate the output and the touch input (README: Portrait screens)" ;;
        1080x1920) ok "  natively portrait: no rotation needed" ;;
    esac
done

echo
echo "Touch devices"
found=0
if [ -r /proc/bus/input/devices ]; then
    # A touch screen reports absolute multi-touch position axes,
    # ABS_MT_POSITION_X/Y = bits 53 and 54 of the ABS bitmap. The kernel
    # prints the bitmap as space-separated words of the CPU's `long` size
    # (32 or 64 bits), most significant first; rebuild it at full width.
    awk -v width="$(( $(getconf LONG_BIT 2>/dev/null || echo 64) / 4 ))" '
        function pad(h) { while (length(h) < width) h = "0" h; return h }
        /^N: Name=/ { name = substr($0, 9); gsub(/"/, "", name) }
        /^B: ABS=/  {
            sub(/^ABS=/, "", $2)
            abs = $2
            for (i = 3; i <= NF; i++) abs = abs pad($i)
        }
        /^$/ {
            # bits 53/54 are bits 1/2 of the 14th hex digit from the right
            if (length(abs) >= 14) {
                digit = index("0123456789abcdef", tolower(substr(abs, length(abs) - 13, 1))) - 1
                if (int(digit / 2) % 4 == 3) print name
            }
            name = ""; abs = ""
        }' /proc/bus/input/devices 2>/dev/null | while read -r dev; do
        echo "  [ok]   touch screen: $dev"
    done | grep . && found=1
fi
if [ "$found" = 0 ]; then
    if have libinput && libinput list-devices 2>/dev/null | grep -q 'Capabilities:.*touch'; then
        libinput list-devices 2>/dev/null | awk '/^Device:/{d=$0} /Capabilities:.*touch/{print "  [ok]   touch screen: " substr(d, 9)}'
    else
        warn "no touch screen detected (is it plugged in? USB touch panels need their USB cable too)"
    fi
fi
if ! id -nG 2>/dev/null | grep -qw input && [ ! -r /dev/input/event0 ]; then
    warn "not in the 'input' group: fine under a desktop or cage, which own the devices"
fi

echo
echo "Next: litebrowser examples/hardware-test.html   (see TESTING.md)"
