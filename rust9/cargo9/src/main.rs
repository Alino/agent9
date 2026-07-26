mod json;
mod registry;
mod toml;

use std::collections::{HashMap, HashSet};
use std::env;
use std::fs;
use std::path::{Path, PathBuf};
use std::process::Command;

fn main() {
    let args: Vec<String> = env::args().collect();
    let cmd = args.get(1).map(|s| s.as_str());
    let rest = &args[2.min(args.len())..];

    let result = match cmd {
        Some("build") => build_local(rest).map(|_| ()),
        Some("run") => run_local(rest),
        Some("clean") => clean_local(),
        Some("selftest") => selftest(),
        Some("install") => rest
            .first()
            .ok_or_else(|| "cargo9: install: usage: cargo9 install <crate>[@version]".to_string())
            .and_then(|spec| registry::install(spec)),
        _ => Err("cargo9: error: unknown subcommand `install` (build|run|clean|selftest)".to_string()),
    };

    if let Err(e) = result {
        eprintln!("{e}");
        std::process::exit(1);
    }
}

// ------------------------------------------------------- local project build
//
// Recreates (equivalently, not byte-for-byte — the original ~182-line
// scratchpad source is lost) cargo9's original offline behavior: read the
// project's own Cargo.toml, resolve `{ path = "..." }` deps (now ALSO plain
// version deps, checked against the registry vendor cache rather than
// fetched live — `build`/`run` never touch the network, only `install`
// does), topologically build each as an rlib, link the root bin.
//
// Also fixes a real bug surfaced while building the `install` feature: a
// package with BOTH src/lib.rs and src/main.rs (common in real crates, e.g.
// `catr`) was previously handled as either/or. Now lib.rs is built first (if
// present) and linked into main.rs via an implicit --extern.

struct LocalPkg {
    manifest: toml::Manifest,
    root: PathBuf,
}

fn load_manifest(dir: &Path) -> Result<toml::Manifest, String> {
    let path = dir.join("Cargo.toml");
    let text = fs::read_to_string(&path).map_err(|e| format!("cargo9: reading {}: {e}", path.display()))?;
    toml::parse(&text).map_err(|e| format!("cargo9: {}: {e}", path.display()))
}

/// Resolves the local dependency graph (path deps recursively, plus version
/// deps looked up in the registry vendor cache — never fetched here) into
/// leaves-first build order. Returns (order, dep-edges keyed by crate root).
fn resolve_local(root_dir: &Path) -> Result<Vec<LocalPkg>, String> {
    let mut order: Vec<LocalPkg> = Vec::new();
    let mut seen: HashSet<PathBuf> = HashSet::new();

    fn visit(dir: &Path, seen: &mut HashSet<PathBuf>, order: &mut Vec<LocalPkg>) -> Result<(), String> {
        let canon = fs::canonicalize(dir).unwrap_or_else(|_| dir.to_path_buf());
        if seen.contains(&canon) {
            return Ok(());
        }
        seen.insert(canon.clone());
        let manifest = load_manifest(dir)?;
        for dep in &manifest.dependencies {
            match &dep.kind {
                toml::DepKind::Path(p) => {
                    let dep_dir = dir.join(p);
                    visit(&dep_dir, seen, order)?;
                }
                toml::DepKind::Version(_) => {
                    // Not fetched here — must already be vendored by a prior
                    // `cargo9 install`. Existence is checked at link time in
                    // build_local via the registry cache path.
                }
                toml::DepKind::Other => {
                    return Err(format!(
                        "cargo9: dependency `{}` in {} is not a path or version dep (git/workspace-inherited deps are not supported)",
                        dep.name,
                        dir.join("Cargo.toml").display()
                    ));
                }
            }
        }
        order.push(LocalPkg { manifest, root: dir.to_path_buf() });
        Ok(())
    }

    visit(root_dir, &mut seen, &mut order)?;
    Ok(order)
}

fn build_dir(root: &Path) -> PathBuf {
    root.join("target").join("9front")
}

/// Builds the local project and returns the path to the linked binary.
fn build_local(_args: &[String]) -> Result<PathBuf, String> {
    let cwd = env::current_dir().map_err(|e| format!("cargo9: {e}"))?;
    let pkgs = resolve_local(&cwd)?;
    if pkgs.is_empty() {
        return Err("cargo9: build: empty dependency graph".to_string());
    }

    let mut rlibs: HashMap<String, PathBuf> = HashMap::new();
    let mut out_bin: Option<PathBuf> = None;

    for (i, pkg) in pkgs.iter().enumerate() {
        let is_root = i == pkgs.len() - 1;
        let out_dir = build_dir(&pkg.root);

        let mut externs: Vec<(String, PathBuf)> = Vec::new();
        let mut has_version_dep = false;
        for dep in &pkg.manifest.dependencies {
            match &dep.kind {
                toml::DepKind::Path(_) => {
                    let rlib = rlibs.get(&dep.name).ok_or_else(|| {
                        format!("cargo9: internal error: path dep `{}` not built before {}", dep.name, pkg.manifest.name)
                    })?;
                    externs.push((registry::normalize(&dep.name), rlib.clone()));
                }
                toml::DepKind::Version(req) => {
                    let rlib = vendored_rlib_for(&dep.name, req)?;
                    externs.push((registry::normalize(&dep.name), rlib));
                    has_version_dep = true;
                }
                toml::DepKind::Other => unreachable!("filtered out during resolve_local"),
            }
        }
        // A vendored (`install`-ed) dep can itself have transitive deps that
        // rustc needs -L coverage for (see compile_rlib's doc comment) — we
        // don't re-run the registry's own dependency resolution here, so
        // ponytail: just add every vendored crate's build dir as a search
        // path rather than re-deriving the exact transitive set. Harmless
        // (extra -L dirs are a no-op if unused); upgrade to precise coverage
        // if a local build ever needs disambiguating between same-named
        // vendored crates.
        let mut search_dirs: Vec<PathBuf> = rlibs.values().filter_map(|p| p.parent().map(Path::to_path_buf)).collect();
        if has_version_dep {
            search_dirs.extend(all_vendored_search_dirs());
        }

        let lib_path = pkg.manifest.lib.as_ref().and_then(|l| l.path.clone()).unwrap_or_else(|| "src/lib.rs".to_string());
        let lib_rs = pkg.root.join(&lib_path);
        let mut own_rlib: Option<PathBuf> = None;
        if lib_rs.exists() {
            let rlib = registry::compile_rlib(&pkg.manifest.name, &pkg.manifest.edition, &lib_rs, &out_dir, &externs, &search_dirs)?;
            rlibs.insert(pkg.manifest.name.clone(), rlib.clone());
            own_rlib = Some(rlib);
        }

        if is_root {
            let bin_path = pkg
                .manifest
                .bins
                .first()
                .map(|b| b.path.clone().unwrap_or_else(|| "src/main.rs".to_string()))
                .unwrap_or_else(|| "src/main.rs".to_string());
            let main_rs = pkg.root.join(&bin_path);
            if !main_rs.exists() {
                return Err(format!("cargo9: build: {} has no src/main.rs (bin) to build", pkg.manifest.name));
            }
            let mut root_externs = externs;
            if let Some(rlib) = &own_rlib {
                root_externs.push((registry::normalize(&pkg.manifest.name), rlib.clone()));
            }
            let binname = pkg.manifest.bins.first().map(|b| b.name.clone()).filter(|n| !n.is_empty()).unwrap_or_else(|| pkg.manifest.name.clone());
            let out = out_dir.join(&binname);
            registry::compile_bin(&pkg.manifest.name, &pkg.manifest.edition, &main_rs, &out, &root_externs, &search_dirs)?;
            out_bin = Some(out);
        } else if lib_rs.exists() {
            // already built above
        } else {
            return Err(format!("cargo9: build: {} (a dependency) has no src/lib.rs", pkg.manifest.name));
        }
    }

    out_bin.ok_or_else(|| "cargo9: build: nothing built".to_string())
}

fn vendored_rlib_for(name: &str, req: &str) -> Result<PathBuf, String> {
    let cargo9_root = env::var("CARGO9_ROOT").unwrap_or_else(|_| "/usr/glenda/rust".to_string());
    let src_dir = PathBuf::from(&cargo9_root).join("registry").join("src");
    let entries = fs::read_dir(&src_dir).map_err(|_| {
        format!("cargo9: dependency `{name}` is a version dep but nothing is vendored — run `cargo9 install {name}` first")
    })?;
    let prefix = format!("{name}-");
    let parsed_req = registry::parse_req(req);
    // Bare prefix matching would let e.g. `log` match a vendored
    // `log-mdc-<ver>` directory. Require the exact `<name>-<semver>` shape,
    // and pick the HIGHEST vendored version that satisfies the requirement
    // deterministically — not "whatever the OS happened to list last."
    let mut best: Option<(registry::SemVer, PathBuf)> = None;
    for entry in entries.flatten() {
        let fname = entry.file_name();
        let fname = fname.to_string_lossy().to_string();
        let Some(ver_str) = fname.strip_prefix(&prefix) else { continue };
        let Some(v) = registry::parse_semver(ver_str) else { continue };
        if !registry::req_matches(&parsed_req, v) {
            continue;
        }
        let better = match &best {
            None => true,
            Some((bv, _)) => v > *bv,
        };
        if better {
            best = Some((v, entry.path()));
        }
    }
    let (_, dir) = best.ok_or_else(|| {
        format!("cargo9: dependency `{name}` (requirement \"{req}\") is not vendored — run `cargo9 install {name}` first")
    })?;
    let rlib = dir.join("target").join("9front-install").join(format!("lib{}.rlib", registry::normalize(name)));
    if !rlib.exists() {
        return Err(format!("cargo9: vendored dependency `{name}` has not been built yet — run `cargo9 install {name}` first"));
    }
    Ok(rlib)
}

fn all_vendored_search_dirs() -> Vec<PathBuf> {
    let cargo9_root = env::var("CARGO9_ROOT").unwrap_or_else(|_| "/usr/glenda/rust".to_string());
    let src_dir = PathBuf::from(&cargo9_root).join("registry").join("src");
    let Ok(entries) = fs::read_dir(&src_dir) else { return Vec::new() };
    entries
        .flatten()
        .map(|e| e.path().join("target").join("9front-install"))
        .filter(|p| p.exists())
        .collect()
}

fn run_local(args: &[String]) -> Result<(), String> {
    let bin = build_local(&[])?;
    let status = Command::new(&bin)
        .args(args)
        .status()
        .map_err(|e| format!("cargo9: run: spawning {}: {e}", bin.display()))?;
    if !status.success() {
        std::process::exit(status.code().unwrap_or(1));
    }
    Ok(())
}

fn clean_local() -> Result<(), String> {
    let cwd = env::current_dir().map_err(|e| format!("cargo9: {e}"))?;
    let dir = build_dir(&cwd);
    if dir.exists() {
        fs::remove_dir_all(&dir).map_err(|e| format!("cargo9: clean: {e}"))?;
    }
    Ok(())
}

fn selftest() -> Result<(), String> {
    let tmp = env::temp_dir().join(format!("cargo9-selftest-{}", std::process::id()));
    fs::create_dir_all(tmp.join("src")).map_err(|e| e.to_string())?;
    fs::write(
        tmp.join("Cargo.toml"),
        "[package]\nname = \"selftest\"\nversion = \"0.0.0\"\nedition = \"2021\"\n\n[[bin]]\nname = \"selftest\"\npath = \"src/main.rs\"\n",
    )
    .map_err(|e| e.to_string())?;
    fs::write(tmp.join("src/main.rs"), "fn main() { println!(\"hi\"); }\n").map_err(|e| e.to_string())?;

    let saved_cwd = env::current_dir().map_err(|e| e.to_string())?;
    env::set_current_dir(&tmp).map_err(|e| e.to_string())?;
    let build_result = build_local(&[]);
    env::set_current_dir(&saved_cwd).map_err(|e| e.to_string())?;

    let bin = build_result?;
    let out = Command::new(&bin).output().map_err(|e| format!("cargo9: selftest: {e}"))?;
    let _ = fs::remove_dir_all(&tmp);
    if out.status.success() && String::from_utf8_lossy(&out.stdout).trim() == "hi" {
        println!("cargo9 selftest OK");
        Ok(())
    } else {
        Err("cargo9: selftest: FAILED".to_string())
    }
}
