// Injected into every page and into the toolbar. It provides:
//  - browser keyboard shortcuts, whichever WebView has focus
//  - client-side route change reports (pages only)
//  - resetting the error page's retry backoff once a page loads
//  - on-screen keyboard support: reports when a text field is tapped, and
//    window.__liteKey() which types the keys the keyboard sends.
// Rust prepends `window.__liteToolbar` and `window.__liteKeyboard`
// ("on" | "touch" | "off") before this script.
(() => {
  if (window.__liteInjected) return;
  window.__liteInjected = true;
  const isToolbar = !!window.__liteToolbar;
  const post = (msg) => window.ipc && window.ipc.postMessage(JSON.stringify(msg));

  // ---- Keyboard shortcuts --------------------------------------------------
  const shortcut = (e) => {
    if (!e.isTrusted) return null;
    if (e.key === "F12") return "toggle-devtools";
    if (e.key === "F5" || (e.ctrlKey && e.key.toLowerCase() === "r")) return "reload";
    if (e.key === "F6" || (e.ctrlKey && e.key.toLowerCase() === "l")) return "focus-address";
    if (e.altKey && e.key === "ArrowLeft") return "back";
    if (e.altKey && e.key === "ArrowRight") return "forward";
    if (e.key === "Escape" && isToolbar) return "stop";
    if (e.ctrlKey && (e.key === "+" || e.key === "=")) return "zoom-in";
    if (e.ctrlKey && e.key === "-") return "zoom-out";
    if (e.ctrlKey && e.key === "0") return "zoom-reset";
    return null;
  };
  window.addEventListener("keydown", (e) => {
    const cmd = shortcut(e);
    if (!cmd) return;
    e.preventDefault();
    e.stopPropagation();
    post({ cmd });
  }, true);

  // ---- Single-page-app routing ----------------------------------------------
  if (!isToolbar) {
    const notify = () => post({ cmd: "url-changed" });
    for (const name of ["pushState", "replaceState"]) {
      const orig = history[name];
      history[name] = function (...args) {
        const result = orig.apply(this, args);
        notify();
        return result;
      };
    }
    window.addEventListener("popstate", notify);
    window.addEventListener("hashchange", notify);
  }

  // ---- Error page retry backoff (ui/error.html) -------------------------------
  // Once a real page loads, the next failure starts retrying quickly again.
  if (!isToolbar) {
    document.addEventListener("DOMContentLoaded", () => {
      if (!("liteError" in document.documentElement.dataset)) {
        try { sessionStorage.removeItem("__litebrowserRetry"); } catch { /* storage disabled */ }
      }
    });
  }

  // ---- On-screen keyboard ----------------------------------------------------
  const mode = window.__liteKeyboard || "off";
  if (mode === "off") return;

  const TEXT_INPUTS = new Set(["", "text", "search", "email", "url", "tel", "password", "number"]);
  const editable = (el) => {
    if (!el || el.disabled || el.readOnly) return false;
    if (el.isContentEditable || el.tagName === "TEXTAREA") return true;
    return el.tagName === "INPUT" && TEXT_INPUTS.has((el.getAttribute("type") || "").toLowerCase());
  };

  // Only pop the keyboard up for focus the user caused by tapping, not for
  // autofocus or focus() calls during page load.
  let lastTap = { time: 0, type: "" };
  window.addEventListener("pointerdown", (e) => { lastTap = { time: Date.now(), type: e.pointerType }; }, true);
  const tappedRecently = () =>
    Date.now() - lastTap.time < 1500 && (mode === "on" || lastTap.type === "touch");

  let shown = false;
  window.addEventListener("focusin", (e) => {
    if (editable(e.target) && tappedRecently()) {
      shown = true;
      post({ cmd: "keyboard", show: true });
    }
  }, true);
  window.addEventListener("focusout", () => {
    // Defer: focus may be moving to another field. When the whole WebView
    // loses focus, activeElement keeps pointing at the field, so it stays up.
    setTimeout(() => {
      if (shown && !editable(document.activeElement)) {
        shown = false;
        post({ cmd: "keyboard", show: false });
      }
    }, 120);
  }, true);

  // Keep the field visible once the page has shrunk to make room.
  window.__liteKeyboardShown = () => {
    const el = document.activeElement;
    if (editable(el)) setTimeout(() => el.scrollIntoView({ block: "center", behavior: "smooth" }), 80);
  };

  // Writes `value` through the native setter so React's onChange fires
  // (React ignores plain `el.value = …` assignments on controlled inputs).
  const setValue = (el, value, caret) => {
    const proto = el.tagName === "TEXTAREA" ? HTMLTextAreaElement.prototype : HTMLInputElement.prototype;
    Object.getOwnPropertyDescriptor(proto, "value").set.call(el, value);
    el.dispatchEvent(new Event("input", { bubbles: true }));
    try { el.setSelectionRange(caret, caret); } catch { /* email/number fields have no caret API */ }
  };
  const selection = (el) => {
    try {
      if (el.selectionStart != null) return [el.selectionStart, el.selectionEnd];
    } catch { /* no caret API */ }
    return [el.value.length, el.value.length];
  };
  const insert = (el, text) => {
    // execCommand produces real beforeinput/input events and keeps undo.
    if (document.execCommand("insertText", false, text)) return;
    if (el.isContentEditable) return;
    const [start, end] = selection(el);
    setValue(el, el.value.slice(0, start) + text + el.value.slice(end), start + text.length);
  };
  const backspace = (el) => {
    if (document.execCommand("delete")) return;
    if (el.isContentEditable) return;
    const [start, end] = selection(el);
    const from = start === end ? Math.max(0, start - 1) : start;
    setValue(el, el.value.slice(0, from) + el.value.slice(end), from);
  };
  const moveCaret = (el, delta) => {
    if (el.isContentEditable) {
      window.getSelection().modify("move", delta < 0 ? "backward" : "forward", "character");
      return;
    }
    const [start] = selection(el);
    const pos = Math.min(el.value.length, Math.max(0, start + delta));
    try { el.setSelectionRange(pos, pos); } catch { /* no caret API */ }
  };

  // Called from Rust with {text: "a"} or {key: "Backspace" | "Enter" | "ArrowLeft" | "ArrowRight"}.
  window.__liteKey = ({ text, key }) => {
    const el = document.activeElement;
    if (!editable(el)) return;
    const name = key || text;
    const event = (type) => el.dispatchEvent(new KeyboardEvent(type, { key: name, bubbles: true, cancelable: true }));
    // Let the page see the keystroke first (e.g. search-on-Enter handlers).
    if (!event("keydown")) { event("keyup"); return; }
    if (text) insert(el, text);
    else if (key === "Backspace") backspace(el);
    else if (key === "ArrowLeft") moveCaret(el, -1);
    else if (key === "ArrowRight") moveCaret(el, 1);
    else if (key === "Enter") {
      if (el.tagName === "TEXTAREA" || el.isContentEditable) insert(el, "\n");
      else if (el.form) el.form.requestSubmit();
    }
    event("keyup");
  };
})();
