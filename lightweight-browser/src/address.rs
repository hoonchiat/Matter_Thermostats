//! Turns whatever the user typed into the address bar into a URL to load.

/// Search engine used when the input does not look like an address.
pub const SEARCH_URL: &str = "https://duckduckgo.com/?q=";

/// Schemes passed through to the WebView unchanged.
const KNOWN_SCHEMES: &[&str] = &["http", "https", "file", "about", "data", "blob"];

/// Normalises address-bar input:
///
/// * `https://example.com`, `file:///tmp/a.html`, `about:blank` → unchanged
/// * `localhost:3000`, `127.0.0.1:5173/app` → `http://…` (dev servers)
/// * `example.com/path` → `https://example.com/path`
/// * `/home/me/index.html` → `file:///home/me/index.html`
/// * anything else → a web search
pub fn resolve(input: &str) -> String {
    let input = input.trim();
    if input.is_empty() {
        return "about:blank".to_string();
    }

    if let Some((scheme, _)) = input.split_once(':') {
        if KNOWN_SCHEMES.contains(&scheme.to_ascii_lowercase().as_str()) {
            return input.to_string();
        }
    }

    if input.starts_with('/') && !input.contains(char::is_whitespace) {
        return format!("file://{input}");
    }

    if !input.contains(char::is_whitespace) {
        let host = host_of(input);
        if is_local_host(host) {
            return format!("http://{input}");
        }
        if looks_like_domain(host) {
            return format!("https://{input}");
        }
    }

    format!("{SEARCH_URL}{}", encode_query(input))
}

/// The `host[:port]` part's host, e.g. `example.com` from `example.com:8080/x?y`.
fn host_of(input: &str) -> &str {
    let authority = input.split(['/', '?', '#']).next().unwrap_or("");
    match authority.rsplit_once(':') {
        Some((host, port)) if !port.is_empty() && port.bytes().all(|b| b.is_ascii_digit()) => host,
        _ => authority,
    }
}

fn is_local_host(host: &str) -> bool {
    host.eq_ignore_ascii_case("localhost") || host.ends_with(".localhost") || host.parse::<std::net::Ipv4Addr>().is_ok()
}

fn looks_like_domain(host: &str) -> bool {
    let labels: Vec<&str> = host.split('.').collect();
    labels.len() >= 2
        && labels.iter().all(|l| {
            !l.is_empty()
                && !l.starts_with('-')
                && !l.ends_with('-')
                && l.chars().all(|c| c.is_alphanumeric() || c == '-')
        })
        && labels
            .last()
            .is_some_and(|tld| tld.chars().all(char::is_alphabetic) && tld.len() >= 2)
}

/// Percent-encodes a search query (`application/x-www-form-urlencoded` style).
fn encode_query(q: &str) -> String {
    let mut out = String::with_capacity(q.len() * 3);
    for b in q.bytes() {
        match b {
            b'A'..=b'Z' | b'a'..=b'z' | b'0'..=b'9' | b'-' | b'_' | b'.' | b'~' => out.push(b as char),
            b' ' => out.push('+'),
            _ => out.push_str(&format!("%{b:02X}")),
        }
    }
    out
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn full_urls_pass_through() {
        assert_eq!(resolve("https://react.dev/learn"), "https://react.dev/learn");
        assert_eq!(resolve("HTTP://Example.com"), "HTTP://Example.com");
        assert_eq!(resolve("file:///tmp/a.html"), "file:///tmp/a.html");
        assert_eq!(resolve("about:blank"), "about:blank");
    }

    #[test]
    fn bare_domains_get_https() {
        assert_eq!(resolve("example.com"), "https://example.com");
        assert_eq!(resolve("  react.dev/learn?x=1  "), "https://react.dev/learn?x=1");
        assert_eq!(resolve("sub.example.co.uk:8443/a"), "https://sub.example.co.uk:8443/a");
    }

    #[test]
    fn dev_servers_get_http() {
        assert_eq!(resolve("localhost:3000"), "http://localhost:3000");
        assert_eq!(resolve("127.0.0.1:5173/app"), "http://127.0.0.1:5173/app");
        assert_eq!(resolve("app.localhost"), "http://app.localhost");
    }

    #[test]
    fn absolute_paths_become_file_urls() {
        assert_eq!(resolve("/home/me/index.html"), "file:///home/me/index.html");
    }

    #[test]
    fn everything_else_is_a_search() {
        assert_eq!(resolve("react hooks"), format!("{SEARCH_URL}react+hooks"));
        assert_eq!(resolve("c++"), format!("{SEARCH_URL}c%2B%2B"));
        assert_eq!(resolve("hello"), format!("{SEARCH_URL}hello"));
        assert_eq!(resolve("1.5"), format!("{SEARCH_URL}1.5"));
    }

    #[test]
    fn empty_input_is_blank_page() {
        assert_eq!(resolve("   "), "about:blank");
    }
}
