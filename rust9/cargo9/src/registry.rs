//! crates.io registry support: `cargo9 install <crate>[@version]`.
//!
//! No HTTP/TLS client lives in cargo9 itself (no external crate for that is
//! available/proven on this port) — fetches shell out to `hget`, which
//! already speaks https on this box (see every pac9 bootstrap doc). The only
//! integrity check is sha256-against-the-index-cksum; there is no TLS chain
//! trust here (hget doesn't provide one). That raises the bar from "tamper
//! the tarball" to "tamper the index AND the tarball consistently" but isn't
//! airtight — see rust9/README.md's install section for the full caveat.
//!
//! This module also hosts the shared rustc-invocation primitives
//! (compile_rlib/compile_bin) used by BOTH the registry build graph here and
//! main.rs's local path-dep orchestrator — it's the same underlying
//! operation (compile a DAG of rlibs, link a bin) fed from two different
//! sources of dependency edges.

use crate::json::{self, Value};
use crate::toml;
use sha2::{Digest, Sha256};
use std::collections::{BTreeSet, HashMap, HashSet, VecDeque};
use std::fs;
use std::io::Write as _;
use std::path::{Path, PathBuf};
use std::process::{Command, Stdio};

const DEFAULT_ROOT: &str = "/usr/glenda/rust";
const DEFAULT_RUSTC: &str = "/usr/glenda/rust/bin/rustc";
const DEFAULT_LINKER: &str = "/usr/glenda/rust/bin/n9link";
// Plan 9's exec(2) does no $PATH search (that's rc's job, not the kernel's) —
// rust9's Command::spawn calls it directly, so a bare "hget" silently fails
// to exec at all (the child's fallback "exec failed" exit gets misdecoded by
// cc9's waitpid as a plain SIGABRT, which looks exactly like a crash and cost
// real debugging time to trace back to "wrong binary name" instead of "child
// crashed"). Always use absolute paths for anything shelled out to.
const DEFAULT_HGET: &str = "/bin/hget";
const DEFAULT_GUNZIP: &str = "/bin/gunzip";
const DEFAULT_TAR: &str = "/bin/tar";

// Proc-macro-machinery crates: if any of these show up anywhere in the
// resolved graph, something needs proc-macros even if its own [lib] block
// didn't say so (defense-in-depth backstop; the primary signal is
// [lib].proc-macro = true on the actual offending crate, checked separately).
const PROC_MACRO_DENYLIST: &[&str] = &[
    "syn",
    "quote",
    "proc-macro2",
    "darling",
    "darling_core",
    "proc-macro-error",
    "proc-macro-crate",
    "venial",
];

pub struct Paths {
    root: PathBuf,
}

impl Paths {
    pub fn new() -> Paths {
        let root = std::env::var("CARGO9_ROOT").unwrap_or_else(|_| DEFAULT_ROOT.to_string());
        Paths { root: PathBuf::from(root) }
    }
    fn registry(&self) -> PathBuf { self.root.join("registry") }
    fn index_cache(&self, name: &str) -> PathBuf { self.registry().join("index").join(name) }
    fn crate_cache(&self, name: &str, version: &str) -> PathBuf {
        self.registry().join("cache").join(format!("{name}-{version}.crate"))
    }
    fn src_dir(&self, name: &str, version: &str) -> PathBuf {
        self.registry().join("src").join(format!("{name}-{version}"))
    }
    fn installed_manifest(&self) -> PathBuf { self.registry().join("installed") }
    fn bin_dir(&self) -> PathBuf { self.root.join("bin") }
    fn rc_bin_dir(&self) -> PathBuf { PathBuf::from("/rc/bin") }
}

fn rustc_path() -> PathBuf {
    PathBuf::from(std::env::var("RUSTC").unwrap_or_else(|_| DEFAULT_RUSTC.to_string()))
}
fn linker_path() -> PathBuf {
    PathBuf::from(std::env::var("RUST9_LINKER").unwrap_or_else(|_| DEFAULT_LINKER.to_string()))
}
fn hget_path() -> PathBuf {
    PathBuf::from(std::env::var("CARGO9_HGET").unwrap_or_else(|_| DEFAULT_HGET.to_string()))
}
fn gunzip_path() -> PathBuf {
    PathBuf::from(std::env::var("CARGO9_GUNZIP").unwrap_or_else(|_| DEFAULT_GUNZIP.to_string()))
}
fn tar_path() -> PathBuf {
    PathBuf::from(std::env::var("CARGO9_TAR").unwrap_or_else(|_| DEFAULT_TAR.to_string()))
}

pub fn normalize(name: &str) -> String {
    name.replace('-', "_")
}

/// Crate names (and rustc bin names) flow, unmodified, into filesystem paths
/// AND a generated `/bin/rc` wrapper script (see `install_to_path`) — never
/// trust one from the index/Cargo.toml without checking its charset first.
/// crates.io itself only accepts this charset for a real published name, but
/// a compromised/MITM'd index response (already an accepted residual risk
/// for THIS purpose, per the module doc comment — but path traversal /
/// command injection is a different, worse consequence, not just a bad
/// build) must not be trusted to have gone through that validation. Applies
/// to crate names, dependency `package` renames, and `[[bin]]` names alike.
fn valid_name(s: &str) -> bool {
    !s.is_empty() && s.len() <= 64 && s.chars().all(|c| c.is_ascii_alphanumeric() || c == '_' || c == '-')
}

/// Same reasoning as `valid_name` — a version string becomes a path
/// component (`registry/src/<name>-<version>/`) and a URL segment.
fn valid_version_str(s: &str) -> bool {
    !s.is_empty() && s.len() <= 64 && s.chars().all(|c| c.is_ascii_alphanumeric() || c == '.' || c == '-' || c == '+')
}

// ---------------------------------------------------------------- semver-lite

/// ponytail: numeric major.minor.patch only, ignoring any -pre/+build
/// suffix for both parsing and ordering. Real semver precedence (pre-release
/// < release) is NOT implemented — upgrade if a resolved graph ever needs it.
#[derive(Debug, Clone, Copy, PartialEq, Eq, PartialOrd, Ord)]
pub(crate) struct SemVer(u64, u64, u64);

pub(crate) fn parse_semver(s: &str) -> Option<SemVer> {
    parse_semver_with_arity(s).map(|(v, _)| v)
}

/// Also reports how many components were actually WRITTEN (1/2/3) — the
/// caret-pinning rule below needs this: `^0` (1 component) means "any 0.x.y",
/// while `^0.0` (2 components) means "any 0.0.z" — same floor (0,0,0), very
/// different ranges. Losing this distinction was a real bug (found live: a
/// bare `^0` dependency requirement was wrongly narrowed to the 0.0.x line,
/// so `cargo9 install choose` couldn't resolve `backslash`'s `^0` req against
/// its real 0.1.x releases).
fn parse_semver_with_arity(s: &str) -> Option<(SemVer, u8)> {
    let core = s.split(['-', '+']).next().unwrap_or(s);
    let mut it = core.split('.');
    let major: u64 = it.next()?.trim().parse().ok()?;
    let minor: u64 = it.next().map(|p| p.trim().parse().unwrap_or(0)).unwrap_or(0);
    let patch: u64 = it.next().map(|p| p.trim().parse().unwrap_or(0)).unwrap_or(0);
    let given = 1 + core.matches('.').count().min(2) as u8;
    Some((SemVer(major, minor, patch), given))
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub(crate) enum PinLevel {
    Major,
    MajorMinor,
    Exact,
}

/// Real cargo caret-pinning rule: pin at the first NONZERO of the given
/// components (major, then minor, then patch); if every given component is
/// zero, pin at whichever component was written LAST (so `^0` — 1 component,
/// all zero — pins at major only, matching `>=0.0.0,<1.0.0`; `^0.0` — 2
/// components — pins at (0,0); `^0.0.3` — 3 components — pins exact).
fn pin_level(floor: SemVer, given: u8) -> PinLevel {
    if floor.0 != 0 {
        PinLevel::Major
    } else if given >= 2 && floor.1 != 0 {
        PinLevel::MajorMinor
    } else if given >= 3 {
        PinLevel::Exact
    } else if given == 2 {
        PinLevel::MajorMinor
    } else {
        PinLevel::Major
    }
}

#[derive(Debug, Clone)]
pub(crate) enum VersionReq {
    Exact(SemVer),
    Compatible(SemVer, PinLevel),
}

/// ponytail: heuristic only — handles `=x.y.z`, bare/`^x.y.z`, `~x.y.z`
/// (treated the same as caret), and best-effort on compound requirements
/// (">=1, <2" — takes the first clause's version as the floor). No real
/// range intersection (e.g. an explicit upper bound on a compound
/// requirement is ignored). Good enough for picking "the newest version
/// that's still obviously compatible," not for exact cargo-resolver parity.
pub(crate) fn parse_req(req: &str) -> VersionReq {
    let req = req.trim();
    let first_clause = req.split(',').next().unwrap_or(req).trim();
    let exact = first_clause.starts_with('=');
    let digits_start = first_clause.find(|c: char| c.is_ascii_digit());
    let ver_str = match digits_start {
        Some(i) => &first_clause[i..],
        None => "0.0.0",
    };
    let (v, given) = parse_semver_with_arity(ver_str).unwrap_or((SemVer(0, 0, 0), 3));
    if exact { VersionReq::Exact(v) } else { VersionReq::Compatible(v, pin_level(v, given)) }
}

pub(crate) fn req_matches(req: &VersionReq, v: SemVer) -> bool {
    match req {
        VersionReq::Exact(e) => *e == v,
        VersionReq::Compatible(floor, pin) => {
            v >= *floor
                && match pin {
                    PinLevel::Major => v.0 == floor.0,
                    PinLevel::MajorMinor => (v.0, v.1) == (floor.0, floor.1),
                    PinLevel::Exact => v == *floor,
                }
        }
    }
}

// -------------------------------------------------------------- index types

#[derive(Debug, Clone)]
struct IndexDep {
    name: String,
    package: Option<String>,
    req: String,
    features: Vec<String>,
    optional: bool,
    default_features: bool,
    target: Option<String>,
    kind: String,
}

impl IndexDep {
    /// The real crate name to fetch (accounts for `package = "..."` renames).
    fn real_name(&self) -> String {
        self.package.clone().unwrap_or_else(|| self.name.clone())
    }
}

#[derive(Debug, Clone)]
struct IndexEntry {
    name: String,
    vers: String,
    deps: Vec<IndexDep>,
    cksum: String,
    yanked: bool,
    /// feature name -> list of implied feature names / `dep:name` /
    /// `pkg?/feat` entries (features + features2 merged by crates.io before
    /// they ever reach the sparse index, so we don't need to merge them).
    features: HashMap<String, Vec<String>>,
}

fn parse_index_entry(v: &Value) -> Option<IndexEntry> {
    let name = v.get("name")?.as_str()?.to_string();
    let vers = v.get("vers")?.as_str()?.to_string();
    // A malformed/malicious name or version becomes a filesystem path and a
    // URL segment later — reject the whole entry here rather than trust it
    // all the way down (see `valid_name`'s doc comment).
    if !valid_name(&name) || !valid_version_str(&vers) {
        return None;
    }
    let cksum = v.get("cksum").and_then(Value::as_str).unwrap_or("").to_string();
    let yanked = v.get("yanked").and_then(Value::as_bool).unwrap_or(false);
    let mut deps = Vec::new();
    if let Some(arr) = v.get("deps").and_then(Value::as_array) {
        for d in arr {
            let dname = match d.get("name").and_then(Value::as_str) {
                Some(n) => n.to_string(),
                None => continue,
            };
            let package = d.get("package").and_then(Value::as_str).map(|s| s.to_string());
            // A bad dep name/rename can't be fixed up — drop just this dep
            // rather than the whole entry (matches the existing skip-based
            // filtering already used for kind/target/optional below).
            if !valid_name(&dname) || package.as_deref().is_some_and(|p| !valid_name(p)) {
                continue;
            }
            let req = d.get("req").and_then(Value::as_str).unwrap_or("*").to_string();
            let optional = d.get("optional").and_then(Value::as_bool).unwrap_or(false);
            let default_features = d.get("default_features").and_then(Value::as_bool).unwrap_or(true);
            let kind = d.get("kind").and_then(Value::as_str).unwrap_or("normal").to_string();
            let target = d.get("target").and_then(Value::as_str).map(|s| s.to_string());
            let features = d
                .get("features")
                .and_then(Value::as_array)
                .map(|a| a.iter().filter_map(Value::as_str).map(|s| s.to_string()).collect())
                .unwrap_or_default();
            deps.push(IndexDep { name: dname, package, req, features, optional, default_features, target, kind });
        }
    }
    let mut features = HashMap::new();
    if let Some(Value::Obj(pairs)) = v.get("features") {
        for (k, val) in pairs {
            if let Some(arr) = val.as_array() {
                features.insert(k.clone(), arr.iter().filter_map(Value::as_str).map(|s| s.to_string()).collect());
            }
        }
    }
    Some(IndexEntry { name, vers, deps, cksum, yanked, features })
}

fn sparse_index_rel_path(name: &str) -> String {
    // Char-indexed, not byte-sliced: every caller validates `name` as ASCII
    // via `valid_name` first, but this stays panic-safe even if that ever
    // changes (byte-slicing a multi-byte UTF-8 boundary panics).
    let lower = name.to_lowercase();
    let chars: Vec<char> = lower.chars().collect();
    match chars.len() {
        0 => "1/_".to_string(), // unreachable given valid_name, kept panic-free anyway
        1 => format!("1/{lower}"),
        2 => format!("2/{lower}"),
        3 => format!("3/{}/{lower}", chars[0]),
        _ => {
            let a: String = chars[0..2].iter().collect();
            let b: String = chars[2..4].iter().collect();
            format!("{a}/{b}/{lower}")
        }
    }
}

fn fetch_index(paths: &Paths, name: &str) -> Result<Vec<IndexEntry>, String> {
    let cache = paths.index_cache(name);
    let bytes = if cache.exists() {
        fs::read(&cache).map_err(|e| format!("install: reading cached index for {name}: {e}"))?
    } else {
        let url = format!("https://index.crates.io/{}", sparse_index_rel_path(name));
        let bytes = hget(&url)?;
        fs::create_dir_all(cache.parent().unwrap())
            .map_err(|e| format!("install: creating index cache dir: {e}"))?;
        fs::write(&cache, &bytes).map_err(|e| format!("install: writing index cache: {e}"))?;
        bytes
    };
    let entries: Vec<IndexEntry> = json::parse_ndjson(&bytes).iter().filter_map(parse_index_entry).collect();
    if entries.is_empty() {
        return Err(format!("install: no index entries found for crate '{name}' (typo, or it doesn't exist on crates.io?)"));
    }
    Ok(entries)
}

fn select_version<'a>(entries: &'a [IndexEntry], req: &VersionReq) -> Result<&'a IndexEntry, String> {
    let mut best: Option<&IndexEntry> = None;
    for e in entries {
        if e.yanked {
            continue;
        }
        let Some(v) = parse_semver(&e.vers) else { continue };
        if !req_matches(req, v) {
            continue;
        }
        let better = match best {
            None => true,
            Some(b) => parse_semver(&b.vers).map(|bv| v > bv).unwrap_or(true),
        };
        if better {
            best = Some(e);
        }
    }
    best.ok_or_else(|| "install: no matching non-yanked version found for the given requirement".to_string())
}

// ------------------------------------------------------------- hget/verify

/// Shells out to `hget`, guarding its known silent-failure mode (a 0-byte
/// file on error, not a nonzero exit) by checking BOTH exit status and
/// output length.
fn hget(url: &str) -> Result<Vec<u8>, String> {
    let out = Command::new(hget_path())
        .arg(url)
        .output()
        .map_err(|e| format!("install: spawning hget for {url}: {e}"))?;
    if !out.status.success() || out.stdout.is_empty() {
        return Err(format!("install: fetch failed (empty or errored): {url}"));
    }
    Ok(out.stdout)
}

fn sha256_hex(bytes: &[u8]) -> String {
    let mut h = Sha256::new();
    h.update(bytes);
    let digest = h.finalize();
    let mut out = String::with_capacity(64);
    for b in digest {
        out.push_str(&format!("{b:02x}"));
    }
    out
}

/// Fetch + checksum-verify + extract one crate. No-op if already extracted
/// (a `.ok` sentinel from a prior successful pass memoizes this).
fn fetch_and_extract(paths: &Paths, entry: &IndexEntry) -> Result<PathBuf, String> {
    let src = paths.src_dir(&entry.name, &entry.vers);
    let sentinel = src.join(".ok");
    if sentinel.exists() {
        return Ok(src);
    }

    let cache_path = paths.crate_cache(&entry.name, &entry.vers);
    let gz_bytes = if cache_path.exists() {
        fs::read(&cache_path).map_err(|e| format!("install: reading cached .crate: {e}"))?
    } else {
        let url = format!("https://static.crates.io/crates/{}/{}-{}.crate", entry.name, entry.name, entry.vers);
        let bytes = hget(&url)?;
        fs::create_dir_all(cache_path.parent().unwrap())
            .map_err(|e| format!("install: creating cache dir: {e}"))?;
        fs::write(&cache_path, &bytes).map_err(|e| format!("install: writing .crate cache: {e}"))?;
        bytes
    };

    if !entry.cksum.is_empty() {
        let actual = sha256_hex(&gz_bytes);
        if !actual.eq_ignore_ascii_case(&entry.cksum) {
            let _ = fs::remove_file(&cache_path);
            return Err(format!(
                "install: checksum mismatch for {}-{}: expected {}, got {} — refusing to extract",
                entry.name, entry.vers, entry.cksum, actual
            ));
        }
    }

    // gunzip, mirroring pac9's own install_tarball(): stdin/stdout redirect,
    // no `-c` flag assumed (9front's gunzip may not have one).
    let tar_scratch = cache_path.with_extension("crate.tar");
    {
        let infile = fs::File::open(&cache_path).map_err(|e| format!("install: opening cached .crate: {e}"))?;
        let outfile = fs::File::create(&tar_scratch).map_err(|e| format!("install: creating tar scratch file: {e}"))?;
        let status = Command::new(gunzip_path())
            .stdin(Stdio::from(infile))
            .stdout(Stdio::from(outfile))
            .status()
            .map_err(|e| format!("install: spawning gunzip: {e}"))?;
        if !status.success() {
            return Err(format!("install: gunzip failed for {}-{}", entry.name, entry.vers));
        }
    }

    fs::create_dir_all(paths.registry().join("src")).map_err(|e| format!("install: creating src dir: {e}"))?;
    // 9front's native tar has no `-C` — use a real per-child chdir instead.
    let status = Command::new(tar_path())
        .arg("xf")
        .arg(&tar_scratch)
        .current_dir(paths.registry().join("src"))
        .status()
        .map_err(|e| format!("install: spawning tar: {e}"))?;
    if !status.success() {
        return Err(format!("install: tar extraction failed for {}-{}", entry.name, entry.vers));
    }
    let _ = fs::remove_file(&tar_scratch);

    if !src.exists() {
        return Err(format!(
            "install: extracted archive for {}-{} did not produce the expected {} directory",
            entry.name, entry.vers, src.display()
        ));
    }
    fs::write(&sentinel, b"ok").map_err(|e| format!("install: writing sentinel: {e}"))?;
    Ok(src)
}

// -------------------------------------------------------------- resolution

struct ResolvedCrate {
    name: String,
    version: String,
    src: PathBuf,
    manifest: toml::Manifest,
    /// (local extern name, real crate name) — after feature/kind/target
    /// filtering, in the order first requested.
    direct_deps: Vec<(String, String)>,
}

struct QueueItem {
    real_name: String,
    req: VersionReq,
    req_str: String, // for error messages
    requested_features: Vec<String>,
    default_features: bool,
    parent: Option<String>,
}

pub struct InstallPlan {
    /// Leaves-first build order; the last entry is the root crate.
    pub order: Vec<ResolvedCrateBuilt>,
}

pub struct ResolvedCrateBuilt {
    pub name: String,
    pub version: String,
    pub src: PathBuf,
    pub manifest: toml::Manifest,
    pub direct_deps: Vec<(String, String)>,
}

/// BFS resolve + fetch + extract the full dependency graph for `spec`
/// ("name" or "name@version"). Returns crates in leaves-first build order.
fn resolve(paths: &Paths, spec: &str) -> Result<InstallPlan, String> {
    let (root_name, root_version) = match spec.split_once('@') {
        Some((n, v)) => (n.to_string(), Some(v.to_string())),
        None => (spec.to_string(), None),
    };
    if !valid_name(&root_name) {
        return Err(format!("install: '{root_name}' is not a valid crate name"));
    }
    if let Some(v) = &root_version {
        if !valid_version_str(v) {
            return Err(format!("install: '{v}' is not a valid version string"));
        }
    }

    let mut resolved: HashMap<String, (String, String)> = HashMap::new(); // real_name -> (version, requirement string that won it)
    let mut built: HashMap<String, ResolvedCrate> = HashMap::new();
    let mut edges: Vec<(String, String)> = Vec::new(); // (parent real_name, child real_name) — "" parent = root
    let mut queue: VecDeque<QueueItem> = VecDeque::new();

    let root_entries = fetch_index(paths, &root_name)?;
    let root_entry = match &root_version {
        Some(v) => {
            let req = VersionReq::Exact(parse_semver(v).ok_or_else(|| format!("install: bad version '{v}'"))?);
            select_version(&root_entries, &req)?
        }
        None => select_latest_nonyanked(&root_entries)?,
    };
    queue.push_back(QueueItem {
        real_name: root_name.clone(),
        req: VersionReq::Exact(parse_semver(&root_entry.vers).unwrap()),
        req_str: root_entry.vers.clone(),
        requested_features: vec![],
        default_features: true,
        parent: None,
    });

    // requested_features accumulated per real_name across the whole BFS run,
    // ponytail: only the accumulation AT FIRST-RESOLUTION time is honored —
    // a later edge that requests a NEW feature on an already-fetched crate
    // does not retroactively re-scan it. Real cargo does full feature
    // unification across the graph; this is the heuristic v1 ships with.
    let mut requested_features: HashMap<String, (BTreeSet<String>, bool)> = HashMap::new();

    while let Some(item) = queue.pop_front() {
        let feat_entry = requested_features.entry(item.real_name.clone()).or_insert_with(|| (BTreeSet::new(), false));
        feat_entry.0.extend(item.requested_features.iter().cloned());
        feat_entry.1 |= item.default_features;

        if let Some(parent) = &item.parent {
            edges.push((parent.clone(), item.real_name.clone()));
        }

        if let Some((existing_version, existing_req)) = resolved.get(&item.real_name) {
            let existing_sv = parse_semver(existing_version).unwrap();
            let new_matches_line = req_matches(&item.req, existing_sv);
            if !new_matches_line {
                return Err(format!(
                    "install: version conflict: {} already resolved to {} (satisfying \"{}\"), but also required as \"{}\" which that version does not satisfy — cargo9 does not support multiple versions of one crate in a build",
                    item.real_name, existing_version, existing_req, item.req_str
                ));
            }
            continue; // already resolved+fetched+scanned; reuse silently
        }

        let entries = fetch_index(paths, &item.real_name)?;
        let entry = select_version(&entries, &item.req)?;
        resolved.insert(item.real_name.clone(), (entry.vers.clone(), item.req_str.clone()));

        let src = fetch_and_extract(paths, entry)?;
        let manifest_text = fs::read_to_string(src.join("Cargo.toml"))
            .map_err(|e| format!("install: reading {}-{} Cargo.toml: {e}", entry.name, entry.vers))?;
        let manifest = toml::parse(&manifest_text)
            .map_err(|e| format!("install: parsing {}-{} Cargo.toml: {e}", entry.name, entry.vers))?;

        let (want_features, want_default) = requested_features
            .get(&item.real_name)
            .cloned()
            .unwrap_or_else(|| (BTreeSet::new(), true));
        let active = activate_features(entry, &want_features, want_default);

        let mut direct_deps = Vec::new();
        for dep in &entry.deps {
            if dep.kind != "normal" {
                continue; // dev/build deps never fetched
            }
            if dep.target.is_some() {
                continue; // cfg-gated deps always skipped in v1
            }
            if dep.optional && !active.contains(&dep.name) {
                continue;
            }
            let real = dep.real_name();
            direct_deps.push((normalize(&dep.name), real.clone()));
            queue.push_back(QueueItem {
                real_name: real,
                req: parse_req(&dep.req),
                req_str: dep.req.clone(),
                requested_features: dep.features.clone(),
                default_features: dep.default_features,
                parent: Some(item.real_name.clone()),
            });
        }

        built.insert(
            item.real_name.clone(),
            ResolvedCrate { name: item.real_name.clone(), version: entry.vers.clone(), src, manifest, direct_deps },
        );
    }

    // Leaves-first topological order via post-order DFS over the collected
    // edges (defense against a bad cycle: track an in-progress set and error
    // instead of infinite-recursing — real crates.io forbids true cycles,
    // this is just a backstop).
    let mut children: HashMap<&str, Vec<&str>> = HashMap::new();
    for (p, c) in &edges {
        children.entry(p.as_str()).or_default().push(c.as_str());
    }
    let mut order: Vec<String> = Vec::new();
    let mut visited: HashSet<String> = HashSet::new();
    let mut in_progress: HashSet<String> = HashSet::new();
    fn visit(
        name: &str,
        children: &HashMap<&str, Vec<&str>>,
        visited: &mut HashSet<String>,
        in_progress: &mut HashSet<String>,
        order: &mut Vec<String>,
    ) -> Result<(), String> {
        if visited.contains(name) {
            return Ok(());
        }
        if in_progress.contains(name) {
            return Err(format!("install: dependency cycle detected involving '{name}'"));
        }
        in_progress.insert(name.to_string());
        if let Some(kids) = children.get(name) {
            for k in kids {
                visit(k, children, visited, in_progress, order)?;
            }
        }
        in_progress.remove(name);
        visited.insert(name.to_string());
        order.push(name.to_string());
        Ok(())
    }
    visit(&root_name, &children, &mut visited, &mut in_progress, &mut order)?;

    let result = order
        .into_iter()
        .map(|name| {
            let rc = built.remove(&name).expect("resolved crate missing from build map");
            ResolvedCrateBuilt { name: rc.name, version: rc.version, src: rc.src, manifest: rc.manifest, direct_deps: rc.direct_deps }
        })
        .collect();

    Ok(InstallPlan { order: result })
}

fn select_latest_nonyanked(entries: &[IndexEntry]) -> Result<&IndexEntry, String> {
    // An unparseable `vers` must be skipped, not treated as a real 0.0.0 —
    // `max_by_key` would otherwise let a malformed entry win "latest," and
    // the immediate `parse_semver(...).unwrap()` right after this call
    // (resolve()'s root-entry path) would then panic on it.
    entries
        .iter()
        .filter(|e| !e.yanked && parse_semver(&e.vers).is_some())
        .max_by_key(|e| parse_semver(&e.vers).unwrap())
        .ok_or_else(|| "install: no parseable, non-yanked version found".to_string())
}

/// ponytail: `dep:name` and `pkg?/feature` (weak-dependency-features, added
/// to Cargo well after this design's baseline) are handled best-effort —
/// `dep:name` is treated as `name`, and `pkg?/feature`/`pkg/feature` are
/// reduced to just `pkg`. Full weak-feature semantics (only forward the
/// specific sub-feature, only if `pkg` is independently enabled) are NOT
/// implemented.
fn activate_features(entry: &IndexEntry, requested: &BTreeSet<String>, default_features: bool) -> BTreeSet<String> {
    let mut active: BTreeSet<String> = BTreeSet::new();
    let mut worklist: VecDeque<String> = VecDeque::new();
    if default_features {
        worklist.push_back("default".to_string());
    }
    for f in requested {
        worklist.push_back(f.clone());
    }
    let mut steps = 0u32;
    while let Some(f) = worklist.pop_front() {
        steps += 1;
        if steps > 4096 {
            break; // malformed/cyclic features table — stop rather than hang
        }
        if !active.insert(f.clone()) {
            continue;
        }
        if let Some(implies) = entry.features.get(&f) {
            for imp in implies {
                let target = imp.strip_prefix("dep:").unwrap_or(imp.as_str());
                let target = target.split("?/").next().unwrap_or(target);
                let target = target.split('/').next().unwrap_or(target);
                worklist.push_back(target.to_string());
            }
        }
    }
    active
}

// ------------------------------------------------------------- refuse-check

/// Recursively collects every `.rs` file under `dir` (a source tree is at
/// most a few dozen files for the crates this feature targets — no need for
/// a depth cap or symlink-loop guard beyond what's already implicit in a
/// freshly-extracted, non-symlinked tarball).
fn find_rs_files(dir: &Path) -> Vec<PathBuf> {
    let mut out = Vec::new();
    let Ok(entries) = fs::read_dir(dir) else { return out };
    for entry in entries.flatten() {
        let p = entry.path();
        if p.is_dir() {
            out.extend(find_rs_files(&p));
        } else if p.extension().is_some_and(|e| e == "rs") {
            out.push(p);
        }
    }
    out
}

fn refuse_check(plan: &InstallPlan) -> Vec<String> {
    let mut problems = Vec::new();

    for c in &plan.order {
        let label = format!("{}-{}", c.name, c.version);

        match &c.manifest.build {
            toml::BuildScript::Disabled | toml::BuildScript::Auto => {}
            toml::BuildScript::Custom(path) => {
                problems.push(format!(
                    "{label} needs a build script ({path}) — not supported; rust9 has no build.rs execution infrastructure"
                ));
            }
        }
        if matches!(c.manifest.build, toml::BuildScript::Auto) && c.src.join("build.rs").exists() {
            problems.push(format!("{label} needs a build script (build.rs) — not supported; rust9 has no build.rs execution infrastructure"));
        }

        if let Some(lib) = &c.manifest.lib {
            if lib.proc_macro {
                problems.push(format!("{label} is a proc-macro crate — rustc on plan9 cannot load proc-macros (no dlopen)"));
            }
        }

        if PROC_MACRO_DENYLIST.contains(&c.name.as_str()) {
            // c itself IS the proc-macro-machinery crate: find who pulled it in.
            let pullers: Vec<&str> = plan
                .order
                .iter()
                .filter(|p| p.direct_deps.iter().any(|(_, real)| real == &c.name))
                .map(|p| p.name.as_str())
                .collect();
            if pullers.is_empty() {
                problems.push(format!("{label} is proc-macro-machinery — rustc on plan9 cannot load proc-macros (no dlopen)"));
            } else {
                problems.push(format!(
                    "{} pulled in by {} — rustc on plan9 cannot load proc-macros (no dlopen)",
                    label,
                    pullers.join(", ")
                ));
            }
        }

        if c.manifest.is_workspace_root {
            problems.push(format!("{label} is a workspace root — not supported"));
        }

        // Real crates keep .rs files under src/ (in nested modules, not just
        // the root) — walk it recursively, not just the crate root (which
        // only ever holds Cargo.toml/README/etc).
        for p in find_rs_files(&c.src.join("src")) {
            if let Ok(text) = fs::read_to_string(&p) {
                if text.contains("asm!") || text.contains("global_asm!") || text.contains("#[naked]") {
                    problems.push(format!(
                        "{label} appears to use asm!/global_asm!/#[naked] in {} — not supported on plan9 (no on-box GNU as); this is a fast pre-check, the real backstop is rustc's own build error",
                        p.display()
                    ));
                }
            }
        }
    }
    problems
}

// ------------------------------------------------------ rustc invocation

/// Compile a `src/lib.rs` into an rlib. `externs` is (local_name, rlib_path)
/// for every DIRECT dependency (gets an explicit --extern). `search_dirs` is
/// every rlib directory in the FULL transitive closure — rustc needs -L
/// coverage for a dependency's OWN dependencies too when loading its
/// metadata, not just the direct edge (confirmed live: omitting a transitive
/// dep's -L dir makes rustc report "can't find crate" for the DIRECT dep
/// whose metadata references it, not the actually-missing transitive one —
/// a confusing error pointing at the wrong crate name). Returns the rlib path.
pub fn compile_rlib(
    name: &str,
    edition: &str,
    lib_rs: &Path,
    out_dir: &Path,
    externs: &[(String, PathBuf)],
    search_dirs: &[PathBuf],
) -> Result<PathBuf, String> {
    fs::create_dir_all(out_dir).map_err(|e| format!("install: creating build dir: {e}"))?;
    let crate_name = normalize(name);
    let out = out_dir.join(format!("lib{crate_name}.rlib"));
    let tmp = out_dir.join(format!("lib{crate_name}.rlib.tmp"));
    let mut cmd = Command::new(rustc_path());
    cmd.arg("--edition").arg(edition);
    cmd.arg("--crate-name").arg(&crate_name);
    cmd.arg("--crate-type").arg("lib");
    cmd.arg("-o").arg(&tmp);
    for dir in dedup_dirs(externs, search_dirs) {
        cmd.arg("-L").arg(dir);
    }
    for (local, path) in externs {
        cmd.arg("--extern").arg(format!("{local}={}", path.display()));
    }
    cmd.arg("-C").arg(format!("linker={}", linker_path().display()));
    cmd.arg(lib_rs);
    run_rustc(cmd, name)?;
    // Compile to a temp name and rename onto the final path only once rustc
    // has actually succeeded — a killed/OOM'd/EAGAIN'd rustc (findings that
    // motivated the RLIMIT_NPROC accounting fix above make this MORE likely,
    // not less) must never leave a truncated file sitting at the exact path
    // `cached.exists()` (build_and_install) later trusts as "already built."
    fs::rename(&tmp, &out).map_err(|e| format!("install: finalizing {}: {e}", out.display()))?;
    Ok(out)
}

/// Compile a `src/main.rs` into the final executable, optionally linking the
/// package's own lib.rs rlib (the dual-target fix) plus every direct dep.
/// See `compile_rlib` for why `search_dirs` must cover the full transitive
/// closure, not just direct deps.
pub fn compile_bin(
    name: &str,
    edition: &str,
    main_rs: &Path,
    out_path: &Path,
    externs: &[(String, PathBuf)],
    search_dirs: &[PathBuf],
) -> Result<(), String> {
    if let Some(dir) = out_path.parent() {
        fs::create_dir_all(dir).map_err(|e| format!("install: creating build dir: {e}"))?;
    }
    let tmp = out_path.with_extension("tmp");
    let mut cmd = Command::new(rustc_path());
    cmd.arg("--edition").arg(edition);
    cmd.arg("--crate-name").arg(normalize(name));
    cmd.arg("--crate-type").arg("bin");
    cmd.arg("-o").arg(&tmp);
    for dir in dedup_dirs(externs, search_dirs) {
        cmd.arg("-L").arg(dir);
    }
    for (local, path) in externs {
        cmd.arg("--extern").arg(format!("{local}={}", path.display()));
    }
    cmd.arg("-C").arg(format!("linker={}", linker_path().display()));
    cmd.arg(main_rs);
    run_rustc(cmd, name)?;
    // Same reasoning as compile_rlib: never let a killed/failed compile
    // leave a truncated a.out at the path `out_bin.exists()` later trusts.
    fs::rename(&tmp, out_path).map_err(|e| format!("install: finalizing {}: {e}", out_path.display()))
}

fn dedup_dirs(externs: &[(String, PathBuf)], search_dirs: &[PathBuf]) -> Vec<PathBuf> {
    let mut seen: HashSet<PathBuf> = HashSet::new();
    let mut out = Vec::new();
    for (_, path) in externs {
        if let Some(dir) = path.parent() {
            if seen.insert(dir.to_path_buf()) {
                out.push(dir.to_path_buf());
            }
        }
    }
    for dir in search_dirs {
        if seen.insert(dir.clone()) {
            out.push(dir.clone());
        }
    }
    out
}

fn run_rustc(mut cmd: Command, crate_label: &str) -> Result<(), String> {
    if std::env::var("CARGO9_DEBUG").is_ok() {
        eprintln!("cargo9: debug: {:?}", cmd);
    }
    let out = cmd.output().map_err(|e| format!("install: spawning rustc for {crate_label}: {e}"))?;
    if !out.status.success() {
        // Don't swallow rustc's own diagnostic — it's the real backstop for
        // anything the refuse-check's static scans missed (asm!, unstable
        // features, etc).
        std::io::stderr().write_all(&out.stderr).ok();
        return Err(format!("install: rustc failed building {crate_label}"));
    }
    Ok(())
}

fn build_dir_for(src: &Path) -> PathBuf {
    src.join("target").join("9front-install")
}

/// Build the whole resolved graph leaves-first, then install the root's
/// binary onto $PATH.
fn build_and_install(paths: &Paths, plan: &InstallPlan) -> Result<String, String> {
    let mut rlibs: HashMap<String, PathBuf> = HashMap::new(); // real crate name -> built rlib path

    for (i, c) in plan.order.iter().enumerate() {
        let is_root = i == plan.order.len() - 1;
        let out_dir = build_dir_for(&c.src);

        let mut externs: Vec<(String, PathBuf)> = Vec::new();
        for (local, real) in &c.direct_deps {
            let rlib = rlibs
                .get(real)
                .ok_or_else(|| format!("install: internal error: {real} not built before its dependent {}", c.name))?;
            externs.push((local.clone(), rlib.clone()));
        }
        // Every rlib dir built so far — leaves-first order guarantees this
        // already covers c's FULL transitive closure, not just direct deps.
        let search_dirs: Vec<PathBuf> = rlibs.values().filter_map(|p| p.parent().map(Path::to_path_buf)).collect();

        let lib_path = c.manifest.lib.as_ref().and_then(|l| l.path.clone()).unwrap_or_else(|| "src/lib.rs".to_string());
        let lib_rs = c.src.join(&lib_path);
        let has_lib = lib_rs.exists();

        let mut own_rlib: Option<PathBuf> = None;
        if has_lib {
            let cached = out_dir.join(format!("lib{}.rlib", normalize(&c.manifest.name)));
            if cached.exists() {
                own_rlib = Some(cached);
            } else {
                own_rlib = Some(compile_rlib(&c.manifest.name, &c.manifest.edition, &lib_rs, &out_dir, &externs, &search_dirs)?);
            }
            rlibs.insert(c.name.clone(), own_rlib.clone().unwrap());
        }

        if is_root {
            let bin_target = c
                .manifest
                .bins
                .first()
                .map(|b| b.path.clone().unwrap_or_else(|| "src/main.rs".to_string()))
                .unwrap_or_else(|| "src/main.rs".to_string());
            let main_rs = c.src.join(&bin_target);
            if !main_rs.exists() {
                if has_lib {
                    return Err(format!(
                        "install: {} is a library-only crate (no [[bin]]/src/main.rs) — nothing to install onto PATH",
                        c.name
                    ));
                }
                return Err(format!("install: {} has no src/main.rs to build", c.name));
            }
            let binname = c.manifest.bins.first().map(|b| b.name.clone()).filter(|n| !n.is_empty()).unwrap_or_else(|| c.manifest.name.clone());
            // binname becomes a path component AND is embedded in a generated
            // `/bin/rc` wrapper script (install_to_path) — a crate whose
            // [[bin]].name contains rc metacharacters or path separators must
            // not reach either. crates.io's own name validation doesn't cover
            // [[bin]].name at all, so this can't be assumed already-checked.
            if !valid_name(&binname) {
                return Err(format!("install: {}'s bin name '{binname}' is not a valid identifier — refusing to install it", c.name));
            }
            let mut root_externs = externs.clone();
            if let Some(rlib) = &own_rlib {
                root_externs.push((normalize(&c.manifest.name), rlib.clone()));
            }
            let out_bin = out_dir.join(&binname);
            if !out_bin.exists() {
                compile_bin(&c.manifest.name, &c.manifest.edition, &main_rs, &out_bin, &root_externs, &search_dirs)?;
            }
            install_to_path(paths, &binname, &out_bin)?;
            return Ok(binname);
        } else if !has_lib {
            return Err(format!("install: {} is a dependency with no src/lib.rs to build", c.name));
        }
    }
    Err("install: empty resolved graph (nothing to build)".to_string())
}

const DEFAULT_CHMOD: &str = "/bin/chmod";
fn chmod_path() -> PathBuf {
    PathBuf::from(std::env::var("CARGO9_CHMOD").unwrap_or_else(|_| DEFAULT_CHMOD.to_string()))
}

/// Marks a file executable. rust9's plan9 target is NOT `cfg(unix)` (its
/// target spec sets `families: []`, deliberately keeping `sys::pal::plan9`
/// rather than the unix backend), so `std::os::unix::fs::PermissionsExt`
/// either doesn't exist for this target or silently compiles out under a
/// `#[cfg(unix)]` guard — either way an install that relied on it left every
/// wrapper script `-rw-rw-r--` (confirmed live: `catr` built and vendored
/// correctly but wasn't executable, and thus invisible to a bare-name PATH
/// lookup, until chmod +x'd by hand). Shell out to the real tool instead.
///
/// Uses an ABSOLUTE mode (`755`), not a relative `+x` — also confirmed live:
/// a rustc `-o`-written file can already come out execute-only
/// (`----x--x--x`, no read bits at all), and `chmod +x` on a file that
/// already HAS its execute bits set is a no-op — it never adds the missing
/// read permission, so the binary still fails to exec with EACCES ("can't
/// read the file to load it"), just with a different, more confusing
/// signature than "not executable."
fn make_executable(path: &Path) -> Result<(), String> {
    let status = Command::new(chmod_path())
        .arg("755")
        .arg(path)
        .status()
        .map_err(|e| format!("install: spawning chmod for {}: {e}", path.display()))?;
    if !status.success() {
        return Err(format!("install: chmod 755 failed for {}", path.display()));
    }
    Ok(())
}

fn install_to_path(paths: &Paths, binname: &str, built: &Path) -> Result<(), String> {
    let bin_dir = paths.bin_dir();
    fs::create_dir_all(&bin_dir).map_err(|e| format!("install: creating {}: {e}", bin_dir.display()))?;
    let dest = bin_dir.join(binname);
    fs::copy(built, &dest).map_err(|e| format!("install: copying binary to {}: {e}", dest.display()))?;
    make_executable(&dest)?;

    let rc_dir = paths.rc_bin_dir();
    fs::create_dir_all(&rc_dir).map_err(|e| format!("install: creating {}: {e}", rc_dir.display()))?;
    let wrapper = rc_dir.join(binname);
    let script = format!("#!/bin/rc\nexec {} $*\n", dest.display());
    fs::write(&wrapper, script).map_err(|e| format!("install: writing wrapper {}: {e}", wrapper.display()))?;
    make_executable(&wrapper)?;
    Ok(())
}

fn record_installed(paths: &Paths, binname: &str, version: &str) -> Result<(), String> {
    let manifest = paths.installed_manifest();
    fs::create_dir_all(paths.registry()).map_err(|e| e.to_string())?;
    let line = format!(
        "{}\t{}\t{}\t{}\n",
        binname,
        version,
        paths.bin_dir().join(binname).display(),
        paths.rc_bin_dir().join(binname).display()
    );
    let mut f = fs::OpenOptions::new()
        .create(true)
        .append(true)
        .open(&manifest)
        .map_err(|e| format!("install: opening {}: {e}", manifest.display()))?;
    f.write_all(line.as_bytes()).map_err(|e| e.to_string())
}

// ------------------------------------------------------------------ public

pub fn install(spec: &str) -> Result<(), String> {
    let paths = Paths::new();
    let plan = resolve(&paths, spec)?;

    let problems = refuse_check(&plan);
    if !problems.is_empty() {
        let mut msg = String::from("cargo9: install: refusing — this crate graph needs features rust9 doesn't support:\n");
        for p in &problems {
            msg.push_str("  - ");
            msg.push_str(p);
            msg.push('\n');
        }
        return Err(msg);
    }

    let root = plan.order.last().ok_or("install: nothing resolved")?;
    let root_version = root.version.clone();
    let binname = build_and_install(&paths, &plan)?;
    record_installed(&paths, &binname, &root_version)?;
    println!("cargo9: installed {binname} {root_version}");
    Ok(())
}

#[cfg(test)]
mod tests {
    use super::*;

    fn v(s: &str) -> SemVer {
        parse_semver(s).unwrap()
    }

    #[test]
    fn bare_zero_caret_matches_the_whole_0x_range() {
        // Regression test: `^0` (found live via `backslash`'s real req in the
        // `choose` crate graph) must match ANY 0.x.y, not just 0.0.x.
        let req = parse_req("^0");
        assert!(req_matches(&req, v("0.1.5")));
        assert!(req_matches(&req, v("0.9.0")));
        assert!(!req_matches(&req, v("1.0.0")));
    }

    #[test]
    fn bare_zero_no_caret_is_the_same_as_caret() {
        let req = parse_req("0");
        assert!(req_matches(&req, v("0.5.0")));
        assert!(!req_matches(&req, v("1.0.0")));
    }

    #[test]
    fn zero_dot_minor_pins_the_minor() {
        let req = parse_req("^0.3");
        assert!(req_matches(&req, v("0.3.9")));
        assert!(!req_matches(&req, v("0.4.0")));
        assert!(!req_matches(&req, v("0.2.9")));
    }

    #[test]
    fn zero_dot_zero_dot_patch_is_exact() {
        let req = parse_req("^0.0.3");
        assert!(req_matches(&req, v("0.0.3")));
        assert!(!req_matches(&req, v("0.0.4")));
    }

    #[test]
    fn major_over_zero_pins_the_major_only() {
        let req = parse_req("^1.2");
        assert!(req_matches(&req, v("1.9.0")));
        assert!(!req_matches(&req, v("2.0.0")));
        assert!(!req_matches(&req, v("1.1.0"))); // below the floor
    }

    #[test]
    fn exact_requirement_matches_only_that_version() {
        let req = parse_req("=1.2.3");
        assert!(req_matches(&req, v("1.2.3")));
        assert!(!req_matches(&req, v("1.2.4")));
    }

    #[test]
    fn version_ordering_is_numeric_not_lexicographic() {
        // 1.9.0 vs 1.10.0 must not string-sort wrong.
        assert!(v("1.10.0") > v("1.9.0"));
    }

    #[test]
    fn sparse_index_prefix_rules() {
        assert_eq!(sparse_index_rel_path("a"), "1/a");
        assert_eq!(sparse_index_rel_path("ab"), "2/ab");
        assert_eq!(sparse_index_rel_path("abc"), "3/a/abc");
        assert_eq!(sparse_index_rel_path("catr"), "ca/tr/catr");
        assert_eq!(sparse_index_rel_path("serde_json"), "se/rd/serde_json");
    }

    #[test]
    fn feature_closure_expands_transitively_and_activates_optional_deps() {
        let mut features = HashMap::new();
        features.insert("default".to_string(), vec!["suggestions".to_string()]);
        features.insert("suggestions".to_string(), vec!["vec_map".to_string()]);
        let entry = IndexEntry {
            name: "clap".to_string(),
            vers: "2.33.3".to_string(),
            deps: vec![],
            cksum: String::new(),
            yanked: false,
            features,
        };
        let active = activate_features(&entry, &BTreeSet::new(), true);
        assert!(active.contains("default"));
        assert!(active.contains("suggestions"));
        assert!(active.contains("vec_map")); // reached transitively, activates the optional dep of that name
        assert!(!active.contains("color")); // never requested/implied
    }

    #[test]
    fn normalize_replaces_hyphens_only() {
        assert_eq!(normalize("serde-json"), "serde_json");
        assert_eq!(normalize("already_snake"), "already_snake");
    }

    #[test]
    fn valid_name_rejects_path_traversal_and_shell_metacharacters() {
        // Regression test: binname/crate-name/package-rename all flow into
        // filesystem paths and a generated `/bin/rc` wrapper script — none
        // of these must ever reach either unchecked.
        assert!(valid_name("catr"));
        assert!(valid_name("serde-json"));
        assert!(valid_name("serde_json"));
        assert!(!valid_name(""));
        assert!(!valid_name("../../etc/passwd"));
        assert!(!valid_name("foo/bar"));
        assert!(!valid_name("foo;rm -rf /"));
        assert!(!valid_name("foo`whoami`"));
        assert!(!valid_name("foo bar"));
        assert!(!valid_name(&"a".repeat(65)));
    }

    #[test]
    fn valid_version_str_rejects_path_traversal() {
        assert!(valid_version_str("1.2.3"));
        assert!(valid_version_str("1.2.3-beta.1+build.5"));
        assert!(!valid_version_str(""));
        assert!(!valid_version_str("../../../etc"));
        assert!(!valid_version_str("1.2.3; rm -rf /"));
    }

    #[test]
    fn sparse_index_rel_path_never_panics_on_non_ascii() {
        // Defense-in-depth: callers validate via valid_name first, but this
        // must stay panic-free (byte-slicing a UTF-8 boundary panics) even
        // if that invariant is ever violated.
        let _ = sparse_index_rel_path("café");
        let _ = sparse_index_rel_path("日本語");
        let _ = sparse_index_rel_path("");
    }

    #[test]
    fn malformed_index_entry_is_rejected_not_trusted() {
        let bad_name = json::parse(br#"{"name":"../evil","vers":"1.0.0","cksum":"","yanked":false}"#).unwrap();
        assert!(parse_index_entry(&bad_name).is_none());
        let bad_vers = json::parse(br#"{"name":"ok","vers":"1.0.0; rm -rf","cksum":"","yanked":false}"#).unwrap();
        assert!(parse_index_entry(&bad_vers).is_none());
    }

    #[test]
    fn malformed_dependency_entry_is_dropped_not_the_whole_crate() {
        let v = json::parse(
            br#"{"name":"ok","vers":"1.0.0","cksum":"","yanked":false,
                "deps":[{"name":"../evil","req":"1","optional":false,"default_features":true,"target":null,"kind":"normal"},
                        {"name":"fine","req":"1","optional":false,"default_features":true,"target":null,"kind":"normal"}]}"#,
        )
        .unwrap();
        let entry = parse_index_entry(&v).unwrap();
        assert_eq!(entry.deps.len(), 1);
        assert_eq!(entry.deps[0].name, "fine");
    }

    #[test]
    fn select_latest_nonyanked_skips_unparseable_versions_instead_of_treating_them_as_zero() {
        let mk = |vers: &str| IndexEntry {
            name: "x".to_string(),
            vers: vers.to_string(),
            deps: vec![],
            cksum: String::new(),
            yanked: false,
            features: HashMap::new(),
        };
        let entries = vec![mk("not-a-version"), mk("1.2.3"), mk("0.9.0")];
        let picked = select_latest_nonyanked(&entries).unwrap();
        assert_eq!(picked.vers, "1.2.3");
    }
}
