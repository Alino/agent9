#!/usr/bin/env python3
"""
run_behavior.py [target] [limit] — run Zig's *upstream* behavior test suite
(vendor/zig/test/behavior/*.zig) on 9front with the patched zig9 compiler and the
minimal plan9 test runner (test/plan9_test_runner.zig).

For each behavior file: `zig test --test-runner <plan9 runner> --test-no-exec` in
the container, deliver the a.out from the host, run it, parse its
"SUMMARY pass=.. fail=.. skip=.." line. Files that don't compile (self-hosted
backend gaps) or crash at runtime (e.g. a panicking test) are recorded distinctly.

  target: qemu (default) | cirno      limit: optional max number of files

Aggregates totals and writes test/parity/manifests/behavior-<target>.json.
"""
import json, os, subprocess, sys, pathlib, re

HERE = pathlib.Path(__file__).resolve().parent
ROOT = HERE.parent
DELIVER = ROOT.parent / "cc9/host/deliver.py"
DOCKER = os.environ.get("ZIG9_DOCKER", "zig9build")
CZIG = os.environ.get("ZIG9_ZIG", "/tmp/zig-out/bin/zig")
CLIB = "/work/zig9/vendor/zig/lib"
RUNNER = "/work/zig9/test/plan9_test_runner.zig"
BEHDIR = ROOT / "vendor/zig/test/behavior"
FLAGS = ["-target", "x86_64-plan9", "-mcpu=x86_64_v2", "-OReleaseSmall"]
TARGETS = {"qemu": ("127.0.0.1", 1717), "cirno": ("192.168.88.159", 17010)}
SUMMARY = re.compile(r"SUMMARY pass=(\d+) fail=(\d+) skip=(\d+)")

def compile_test(name):
    cout = f"/work/zig9/test/_out/bt_{name}.aout"
    # Compile through a root that *imports* behavior/<name>.zig (at test/ level)
    # rather than compiling the file directly. This matches the upstream
    # test/behavior.zig layout so @typeName yields "behavior.<name>.X" — several
    # meta-tests (typename, string_literals) hardcode those names. (The file's own
    # std-relative imports are unaffected.)
    root = f"/work/zig9/vendor/zig/test/_zig9_{name}.zig"
    subprocess.run(["docker", "exec", DOCKER, "sh", "-c",
                    f'printf "comptime {{ _ = @import(\\"behavior/{name}.zig\\"); }}\\n" > {root}'],
                   capture_output=True, text=True)
    r = subprocess.run(
        ["docker", "exec", DOCKER, CZIG, "test", root,
         "--test-runner", RUNNER, "--zig-lib-dir", CLIB, *FLAGS,
         "--test-no-exec", f"-femit-bin={cout}"],
        capture_output=True, text=True)
    host_out = HERE / "_out" / f"bt_{name}.aout"
    ok = host_out.exists() and host_out.stat().st_size > 0
    err = ""
    if not ok:
        err = next((l for l in (r.stderr or "").splitlines()
                    if "error:" in l or "panic:" in l), (r.stderr or "")[:160])
    return ok, str(host_out), err

def run_test(aout, host, port):
    try:
        out = subprocess.run([sys.executable, str(DELIVER), aout, host, str(port)],
                             capture_output=True, text=True, timeout=90).stdout
    except Exception as e:
        return None, f"<deliver error {e}>"
    return out, None

def main():
    target = sys.argv[1] if len(sys.argv) > 1 else "qemu"
    limit = int(sys.argv[2]) if len(sys.argv) > 2 else 10**9
    host, port = TARGETS[target]
    (HERE / "_out").mkdir(exist_ok=True)
    files = sorted(p.stem for p in BEHDIR.glob("*.zig"))[:limit]
    rows, tot_p, tot_f, tot_s = [], 0, 0, 0
    n_compile_fail = n_crash = n_ok_files = 0
    for name in files:
        cok, aout, cerr = compile_test(name)
        if not cok:
            rows.append({"file": name, "status": "compile-fail", "detail": cerr})
            n_compile_fail += 1
            print(f"  CC-FAIL  {name}  :: {cerr[:70]}")
            continue
        out, rerr = run_test(aout, host, port)
        m = SUMMARY.search(out or "")
        if not m:
            rows.append({"file": name, "status": "crash", "detail": (out or rerr or "").strip()[:120]})
            n_crash += 1
            print(f"  CRASH    {name}  :: {(out or rerr or '').strip()[:60]!r}")
            continue
        pf, ff, sf = map(int, m.groups())
        tot_p += pf; tot_f += ff; tot_s += sf; n_ok_files += 1
        rows.append({"file": name, "status": "ran", "pass": pf, "fail": ff, "skip": sf})
        flag = "" if ff == 0 else "  <-- has failures"
        print(f"  RAN      {name}  pass={pf} fail={ff} skip={sf}{flag}")

    manifest = {
        "target": target, "host": f"{host}:{port}",
        "files_total": len(files), "files_ran": n_ok_files,
        "files_compile_fail": n_compile_fail, "files_crash": n_crash,
        "tests_pass": tot_p, "tests_fail": tot_f, "tests_skip": tot_s,
        "results": rows,
    }
    mdir = HERE / "parity/manifests"; mdir.mkdir(parents=True, exist_ok=True)
    (mdir / f"behavior-{target}.json").write_text(json.dumps(manifest, indent=2) + "\n")
    print(f"\n=== {target}: {n_ok_files}/{len(files)} files ran "
          f"({n_compile_fail} compile-fail, {n_crash} crash) ===")
    print(f"=== upstream behavior tests: {tot_p} pass, {tot_f} fail, {tot_s} skip ===")
    print(f"-> test/parity/manifests/behavior-{target}.json")

if __name__ == "__main__":
    main()
