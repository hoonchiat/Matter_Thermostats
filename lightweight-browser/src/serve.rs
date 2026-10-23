//! Minimal static file server bound to 127.0.0.1, used when the browser is
//! pointed at a local folder or file (e.g. a React production build).
//!
//! Serving over http:// instead of file:// matters for single-page apps:
//! absolute asset paths (`/assets/index-abc.js`) resolve, ES modules and
//! `fetch()` are not blocked by file:// origin rules, and client-side routes
//! (`/settings/profile`) fall back to `index.html` like on a real host.

use std::{
    fs,
    io::{BufRead, BufReader, Write},
    net::{TcpListener, TcpStream},
    path::{Component, Path, PathBuf},
    thread,
};

/// Starts serving `root` on an ephemeral port and returns the base URL
/// (`http://127.0.0.1:PORT`). The server lives for the rest of the process.
pub fn start(root: PathBuf) -> std::io::Result<String> {
    let listener = TcpListener::bind("127.0.0.1:0")?;
    let base = format!("http://{}", listener.local_addr()?);
    thread::spawn(move || {
        for stream in listener.incoming().flatten() {
            let root = root.clone();
            thread::spawn(move || {
                let _ = handle(stream, &root);
            });
        }
    });
    Ok(base)
}

fn handle(mut stream: TcpStream, root: &Path) -> std::io::Result<()> {
    let mut reader = BufReader::new(stream.try_clone()?);
    let mut request_line = String::new();
    reader.read_line(&mut request_line)?;
    // Drain the headers; nothing in them changes what we send back.
    let mut line = String::new();
    while reader.read_line(&mut line)? > 2 {
        line.clear();
    }

    let mut parts = request_line.split_whitespace();
    let (method, target) = (parts.next().unwrap_or(""), parts.next().unwrap_or("/"));
    if method != "GET" && method != "HEAD" {
        return respond(
            &mut stream,
            "405 Method Not Allowed",
            "text/plain",
            b"method not allowed",
            true,
        );
    }

    match resolve(root, target).and_then(|p| fs::read(&p).ok().map(|body| (p, body))) {
        Some((path, body)) => respond(&mut stream, "200 OK", content_type(&path), &body, method == "GET"),
        None => respond(
            &mut stream,
            "404 Not Found",
            "text/plain",
            b"not found",
            method == "GET",
        ),
    }
}

fn respond(stream: &mut TcpStream, status: &str, ctype: &str, body: &[u8], with_body: bool) -> std::io::Result<()> {
    write!(
        stream,
        "HTTP/1.1 {status}\r\nContent-Type: {ctype}\r\nContent-Length: {}\r\nCache-Control: no-cache\r\nConnection: close\r\n\r\n",
        body.len()
    )?;
    if with_body {
        stream.write_all(body)?;
    }
    stream.flush()
}

/// Maps a request target to a file under `root`:
/// * `/` and directories → their `index.html`
/// * a missing path without a file extension → `root/index.html` (SPA route)
/// * anything escaping `root` → `None`
pub fn resolve(root: &Path, target: &str) -> Option<PathBuf> {
    let path = target.split(['?', '#']).next().unwrap_or("/");
    let decoded = percent_decode(path)?;

    let mut file = root.to_path_buf();
    for comp in Path::new(&decoded).components() {
        match comp {
            Component::Normal(part) => file.push(part),
            Component::RootDir | Component::CurDir => {}
            Component::ParentDir | Component::Prefix(_) => return None,
        }
    }

    if file.is_dir() {
        file.push("index.html");
    }
    if file.is_file() {
        // Refuse symlinks that point outside the served folder.
        let (real, real_root) = (file.canonicalize().ok()?, root.canonicalize().ok()?);
        return real.starts_with(real_root).then_some(file);
    }
    if file.extension().is_none() {
        let index = root.join("index.html");
        return index.is_file().then_some(index);
    }
    None
}

/// Percent-encodes a path for use in a URL, keeping `/` separators.
pub fn encode_path(path: &str) -> String {
    let mut out = String::with_capacity(path.len());
    for b in path.bytes() {
        match b {
            b'A'..=b'Z' | b'a'..=b'z' | b'0'..=b'9' | b'-' | b'_' | b'.' | b'~' | b'/' => out.push(b as char),
            _ => out.push_str(&format!("%{b:02X}")),
        }
    }
    out
}

fn percent_decode(s: &str) -> Option<String> {
    let bytes = s.as_bytes();
    let mut out = Vec::with_capacity(bytes.len());
    let mut i = 0;
    while i < bytes.len() {
        if bytes[i] == b'%' {
            let hex = std::str::from_utf8(bytes.get(i + 1..i + 3)?).ok()?;
            out.push(u8::from_str_radix(hex, 16).ok()?);
            i += 3;
        } else {
            out.push(bytes[i]);
            i += 1;
        }
    }
    let decoded = String::from_utf8(out).ok()?;
    (!decoded.contains('\0')).then_some(decoded)
}

pub fn content_type(path: &Path) -> &'static str {
    let ext = path
        .extension()
        .and_then(|e| e.to_str())
        .unwrap_or("")
        .to_ascii_lowercase();
    match ext.as_str() {
        "html" | "htm" => "text/html; charset=utf-8",
        "js" | "mjs" | "cjs" | "jsx" => "text/javascript; charset=utf-8",
        "css" => "text/css; charset=utf-8",
        "json" | "map" => "application/json",
        "webmanifest" => "application/manifest+json",
        "wasm" => "application/wasm",
        "svg" => "image/svg+xml",
        "png" => "image/png",
        "jpg" | "jpeg" => "image/jpeg",
        "gif" => "image/gif",
        "webp" => "image/webp",
        "avif" => "image/avif",
        "ico" => "image/x-icon",
        "woff" => "font/woff",
        "woff2" => "font/woff2",
        "ttf" => "font/ttf",
        "otf" => "font/otf",
        "mp4" => "video/mp4",
        "webm" => "video/webm",
        "mp3" => "audio/mpeg",
        "ogg" => "audio/ogg",
        "wav" => "audio/wav",
        "txt" => "text/plain; charset=utf-8",
        "xml" => "application/xml",
        "pdf" => "application/pdf",
        _ => "application/octet-stream",
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn site(name: &str) -> PathBuf {
        let dir = std::env::temp_dir().join(format!("litebrowser-{name}-{}", std::process::id()));
        let _ = fs::remove_dir_all(&dir);
        fs::create_dir_all(dir.join("assets")).unwrap();
        fs::create_dir_all(dir.join("docs")).unwrap();
        fs::write(dir.join("index.html"), "<div id=root></div>").unwrap();
        fs::write(dir.join("assets/app.js"), "console.log(1)").unwrap();
        fs::write(dir.join("assets/my file.css"), "body{}").unwrap();
        fs::write(dir.join("docs/index.html"), "docs").unwrap();
        dir
    }

    #[test]
    fn resolves_files_dirs_and_spa_routes() {
        let root = site("resolve");
        assert_eq!(resolve(&root, "/"), Some(root.join("index.html")));
        assert_eq!(resolve(&root, "/assets/app.js?v=3"), Some(root.join("assets/app.js")));
        assert_eq!(
            resolve(&root, "/assets/my%20file.css"),
            Some(root.join("assets/my file.css"))
        );
        assert_eq!(resolve(&root, "/docs/"), Some(root.join("docs/index.html")));
        assert_eq!(resolve(&root, "/settings/profile"), Some(root.join("index.html")));
        assert_eq!(resolve(&root, "/assets/missing.js"), None);
        fs::remove_dir_all(root).unwrap();
    }

    #[test]
    fn refuses_to_escape_root() {
        let root = site("escape");
        assert_eq!(resolve(&root, "/../etc/passwd"), None);
        assert_eq!(resolve(&root, "/assets/%2e%2e/%2e%2e/etc/passwd"), None);
        assert_eq!(resolve(&root, "/a%00b"), None);
        assert_eq!(resolve(&root, "/bad%zz"), None);
        fs::remove_dir_all(root).unwrap();
    }

    #[test]
    fn encode_round_trips() {
        let name = "my page #1 ü.html";
        assert_eq!(encode_path(name), "my%20page%20%231%20%C3%BC.html");
        assert_eq!(percent_decode(&encode_path(name)).as_deref(), Some(name));
    }

    #[test]
    fn content_types() {
        assert_eq!(content_type(Path::new("a.MJS")), "text/javascript; charset=utf-8");
        assert_eq!(content_type(Path::new("a.wasm")), "application/wasm");
        assert_eq!(content_type(Path::new("noext")), "application/octet-stream");
    }
}
