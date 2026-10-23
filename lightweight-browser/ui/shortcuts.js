// Injected into every page (and the toolbar) so browser shortcuts work no
// matter which WebView has keyboard focus. In pages it also reports
// client-side route changes.
(() => {
  if (window.__liteShortcuts) return;
  window.__liteShortcuts = true;
  const mac = navigator.platform.toUpperCase().includes("MAC");
  const action = (e) => {
    const mod = mac ? e.metaKey : e.ctrlKey;
    if (e.key === "F12") return "toggle-devtools";
    if (e.key === "F5" || (mod && e.key.toLowerCase() === "r")) return "reload";
    if (e.key === "F6" || (mod && e.key.toLowerCase() === "l")) return "focus-address";
    if (e.altKey && e.key === "ArrowLeft") return "back";
    if (e.altKey && e.key === "ArrowRight") return "forward";
    if (e.key === "Escape" && window.__liteToolbar) return "stop";
    if (mod && (e.key === "+" || e.key === "=")) return "zoom-in";
    if (mod && e.key === "-") return "zoom-out";
    if (mod && e.key === "0") return "zoom-reset";
    return null;
  };
  window.addEventListener("keydown", (e) => {
    const cmd = action(e);
    if (!cmd || !window.ipc) return;
    e.preventDefault();
    e.stopPropagation();
    window.ipc.postMessage(JSON.stringify({ cmd }));
  }, true);

  // Keep the address bar in sync with single-page-app routing.
  if (!window.__liteToolbar) {
    const notify = () => window.ipc && window.ipc.postMessage('{"cmd":"url-changed"}');
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
})();
