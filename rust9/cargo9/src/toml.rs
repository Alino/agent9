//! Hand-rolled, deliberately minimal Cargo.toml scanner.
//!
//! ponytail: this is a line-oriented section scanner, not a real TOML
//! parser (no multi-line arrays/strings, no dotted keys, no arbitrary
//! nesting). It's scoped to exactly the shapes `cargo package` actually
//! emits (for registry-fetched manifests) and to plain hand-written
//! local Cargo.toml files using ordinary `[section]` / `key = value` /
//! `key = { a = b, c = d }` forms. Upgrade to a real parser if a manifest
//! trips it up.

#[derive(Debug, Default)]
pub struct Manifest {
    pub name: String,
    pub version: String,
    pub edition: String,
    pub build: BuildScript,
    pub lib: Option<LibTarget>,
    pub bins: Vec<BinTarget>,
    /// Only meaningful for a LOCAL project's own Cargo.toml (build/run/clean).
    /// Registry-fetched manifests get their dependency graph from the sparse
    /// index JSON instead (already-structured, authoritative) — see registry.rs.
    pub dependencies: Vec<Dep>,
    pub is_workspace_root: bool,
}

#[derive(Debug, Default)]
pub enum BuildScript {
    #[default]
    Auto,
    Disabled,
    Custom(String),
}

#[derive(Debug, Default)]
pub struct LibTarget {
    pub name: Option<String>,
    pub path: Option<String>,
    pub proc_macro: bool,
}

#[derive(Debug, Default)]
pub struct BinTarget {
    pub name: String,
    pub path: Option<String>,
}

#[derive(Debug)]
pub enum DepKind {
    Path(String),
    Version(String),
    /// git/workspace-inherited/anything else cargo9 doesn't understand.
    Other,
}

#[derive(Debug)]
pub struct Dep {
    pub name: String,
    pub kind: DepKind,
}

pub fn parse(text: &str) -> Result<Manifest, String> {
    let mut m = Manifest::default();
    let mut section = String::new();
    let mut current_bin: Option<BinTarget> = None;

    for raw_line in text.lines() {
        let line = strip_comment(raw_line).trim().to_string();
        if line.is_empty() {
            continue;
        }
        if let Some(inner) = line.strip_prefix("[[").and_then(|s| s.strip_suffix("]]")) {
            if let Some(b) = current_bin.take() {
                m.bins.push(b);
            }
            section = inner.trim().to_string();
            if section == "bin" {
                current_bin = Some(BinTarget::default());
            }
            continue;
        }
        if let Some(inner) = line.strip_prefix('[').and_then(|s| s.strip_suffix(']')) {
            if let Some(b) = current_bin.take() {
                m.bins.push(b);
            }
            section = inner.trim().to_string();
            continue;
        }

        let Some(eq) = line.find('=') else { continue };
        let key = line[..eq].trim();
        let val = line[eq + 1..].trim();

        match section.as_str() {
            "package" => match key {
                "name" => m.name = parse_bare_string(val),
                "version" => m.version = parse_bare_string(val),
                "edition" => m.edition = parse_bare_string(val),
                "build" => {
                    m.build = match val {
                        "false" => BuildScript::Disabled,
                        "true" => BuildScript::Auto,
                        _ => BuildScript::Custom(parse_bare_string(val)),
                    }
                }
                _ => {}
            },
            "lib" => {
                let lib = m.lib.get_or_insert_with(LibTarget::default);
                match key {
                    "name" => lib.name = Some(parse_bare_string(val)),
                    "path" => lib.path = Some(parse_bare_string(val)),
                    "proc-macro" | "proc_macro" => lib.proc_macro = val.trim() == "true",
                    _ => {}
                }
            }
            "bin" => {
                if let Some(b) = current_bin.as_mut() {
                    match key {
                        "name" => b.name = parse_bare_string(val),
                        "path" => b.path = Some(parse_bare_string(val)),
                        _ => {}
                    }
                }
            }
            // Exact match only — must NOT catch "dev-dependencies" /
            // "build-dependencies" / "target.'cfg(...)'.dependencies".
            "dependencies" => {
                m.dependencies.push(parse_dependency(key, val));
            }
            "workspace" => {
                if key == "members" {
                    let items = parse_string_array(val);
                    m.is_workspace_root = !items.is_empty();
                }
            }
            _ => {}
        }
    }
    if let Some(b) = current_bin.take() {
        m.bins.push(b);
    }

    if m.name.is_empty() {
        return Err("Cargo.toml: missing [package].name".to_string());
    }
    if m.version.is_empty() {
        m.version = "0.0.0".to_string();
    }
    if m.edition.is_empty() {
        m.edition = "2015".to_string();
    }
    Ok(m)
}

fn parse_dependency(name: &str, val: &str) -> Dep {
    let name = name.to_string();
    if val.starts_with('{') {
        let fields = parse_inline_table(val);
        if let Some((_, v)) = fields.iter().find(|(k, _)| k == "path") {
            return Dep { name, kind: DepKind::Path(parse_bare_string(v)) };
        }
        if let Some((_, v)) = fields.iter().find(|(k, _)| k == "version") {
            return Dep { name, kind: DepKind::Version(parse_bare_string(v)) };
        }
        return Dep { name, kind: DepKind::Other };
    }
    if val.starts_with('"') || val.starts_with('\'') {
        return Dep { name, kind: DepKind::Version(parse_bare_string(val)) };
    }
    Dep { name, kind: DepKind::Other }
}

/// Splits the inside of `{ a = 1, b = [2, 3], c = "x,y" }` on top-level
/// commas (respecting `[...]`/quote nesting) and returns raw key/value pairs.
/// Caller re-parses each value with `parse_bare_string` as needed.
fn parse_inline_table(s: &str) -> Vec<(String, String)> {
    let inner = s.trim().trim_start_matches('{').trim_end_matches('}');
    let mut out = Vec::new();
    let mut depth = 0i32;
    let mut in_str: Option<char> = None;
    let mut start = 0usize;
    let bytes: Vec<char> = inner.chars().collect();
    let push_field = |chunk: &str, out: &mut Vec<(String, String)>| {
        if let Some(eq) = chunk.find('=') {
            let k = chunk[..eq].trim().to_string();
            let v = chunk[eq + 1..].trim().to_string();
            if !k.is_empty() {
                out.push((k, v));
            }
        }
    };
    for (i, &c) in bytes.iter().enumerate() {
        match in_str {
            Some(q) => {
                if c == q {
                    in_str = None;
                }
            }
            None => match c {
                '"' | '\'' => in_str = Some(c),
                '[' => depth += 1,
                ']' => depth -= 1,
                ',' if depth == 0 => {
                    let chunk: String = bytes[start..i].iter().collect();
                    push_field(&chunk, &mut out);
                    start = i + 1;
                }
                _ => {}
            },
        }
    }
    let tail: String = bytes[start..].iter().collect();
    push_field(&tail, &mut out);
    out
}

fn parse_string_array(val: &str) -> Vec<String> {
    let inner = val.trim().trim_start_matches('[').trim_end_matches(']');
    inner
        .split(',')
        .map(|s| parse_bare_string(s.trim()))
        .filter(|s| !s.is_empty())
        .collect()
}

fn parse_bare_string(val: &str) -> String {
    let v = val.trim();
    let v = v.strip_suffix(',').unwrap_or(v).trim();
    if (v.starts_with('"') && v.ends_with('"') && v.len() >= 2)
        || (v.starts_with('\'') && v.ends_with('\'') && v.len() >= 2)
    {
        v[1..v.len() - 1].to_string()
    } else {
        v.to_string()
    }
}

/// Strips a `#` comment, but only when it's not inside a quoted string.
fn strip_comment(line: &str) -> &str {
    let mut in_str: Option<char> = None;
    for (i, c) in line.char_indices() {
        match in_str {
            Some(q) => {
                if c == q {
                    in_str = None;
                }
            }
            None => match c {
                '"' | '\'' => in_str = Some(c),
                '#' => return &line[..i],
                _ => {}
            },
        }
    }
    line
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn parses_path_and_version_and_inline_table_deps() {
        let text = r#"
[package]
name = "app"
version = "0.1.0"
edition = "2021"

[dependencies]
greet = { path = "../greet" }
regex = "1.10"
serde = { version = "1", features = ["derive"] }
gitdep = { git = "https://example.com/x" }

[[bin]]
name = "app"
path = "src/main.rs"
"#;
        let m = parse(text).unwrap();
        assert_eq!(m.name, "app");
        assert_eq!(m.edition, "2021");
        assert_eq!(m.dependencies.len(), 4);
        assert!(matches!(&m.dependencies[0].kind, DepKind::Path(p) if p == "../greet"));
        assert!(matches!(&m.dependencies[1].kind, DepKind::Version(v) if v == "1.10"));
        assert!(matches!(&m.dependencies[2].kind, DepKind::Version(v) if v == "1"));
        assert!(matches!(&m.dependencies[3].kind, DepKind::Other));
        assert_eq!(m.bins.len(), 1);
        assert_eq!(m.bins[0].name, "app");
    }

    #[test]
    fn dev_and_build_dependencies_sections_are_not_mistaken_for_dependencies() {
        let text = r#"
[package]
name = "x"
version = "0.1.0"

[dependencies]
real = "1"

[dev-dependencies]
assert_cmd = "2"

[build-dependencies]
cc = "1"
"#;
        let m = parse(text).unwrap();
        assert_eq!(m.dependencies.len(), 1);
        assert_eq!(m.dependencies[0].name, "real");
    }

    #[test]
    fn build_script_and_proc_macro_and_workspace_fields() {
        let text = r#"
[package]
name = "x"
version = "0.1.0"
build = false

[lib]
proc-macro = true

[workspace]
members = ["a", "b"]
"#;
        let m = parse(text).unwrap();
        assert!(matches!(m.build, BuildScript::Disabled));
        assert!(m.lib.unwrap().proc_macro);
        assert!(m.is_workspace_root);
    }

    #[test]
    fn missing_edition_defaults_to_2015() {
        let text = "[package]\nname = \"x\"\nversion = \"0.1.0\"\n";
        let m = parse(text).unwrap();
        assert_eq!(m.edition, "2015");
    }

    #[test]
    fn comment_inside_string_is_not_stripped() {
        let text = "[package]\nname = \"x # not a comment\"\nversion = \"0.1.0\"\n";
        let m = parse(text).unwrap();
        assert_eq!(m.name, "x # not a comment");
    }
}
