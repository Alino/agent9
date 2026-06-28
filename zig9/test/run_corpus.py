#!/usr/bin/env python3
"""
run_corpus.py [target] — cross-compile each test/corpus/*.zig for x86_64-plan9
with the patched zig9 compiler, run it on a 9front box over listen1, and score
it against its self-check.

The patched compiler (self-hosted-backend + Plan9-linker fixes) is built inside
the Linux container — see port/plan9/linux-build.sh and port/plan9/README.md.
Set ZIG9_ZIG to override the compiler invocation; the default runs the
container build at /tmp/zig-out/bin/zig via `docker exec zig9build`.

Each corpus program is self-checking: on success it prints exactly "ok <name>".
A test PASSES iff it compiles, runs, and stdout contains "ok <name>" with no
"FAIL". Compile failures are recorded distinctly.

  target: "qemu" (127.0.0.1:1717, default) or "cirno" (192.168.88.159:17010)

Writes test/parity/manifests/<target>.json and prints a summary.
"""
import json, os, subprocess, sys, pathlib

HERE = pathlib.Path(__file__).resolve().parent
ROOT = HERE.parent                      # zig9/
DELIVER = ROOT.parent / "cc9/host/deliver.py"
DOCKER = os.environ.get("ZIG9_DOCKER", "zig9build")
CZIG = os.environ.get("ZIG9_ZIG", "/tmp/zig-out/bin/zig")
CLIB = "/work/zig9/vendor/zig/lib"      # container path to patched std
COUT = "/work/zig9/test/_out"           # mounted -> host test/_out
FLAGS = ["-target", "x86_64-plan9", "-mcpu=x86_64_v2", "-OReleaseSmall"]

TARGETS = {"qemu": ("127.0.0.1", 1717), "cirno": ("192.168.88.159", 17010)}

def zig_version():
    r = subprocess.run(["docker", "exec", DOCKER, CZIG, "version"], capture_output=True, text=True)
    return r.stdout.strip() or "unknown"

def build(name):
    """Compile corpus/<name>.zig inside the container; a.out lands in host test/_out."""
    r = subprocess.run(
        ["docker", "exec", DOCKER, CZIG, "build-exe",
         f"/work/zig9/test/corpus/{name}.zig", "--zig-lib-dir", CLIB,
         *FLAGS, f"-femit-bin={COUT}/{name}.aout"],
        capture_output=True, text=True)
    out = HERE / "_out" / f"{name}.aout"
    ok = out.exists() and out.stat().st_size > 0
    return ok, (r.stderr or r.stdout), str(out)

def run_on(aout, host, port):
    r = subprocess.run([sys.executable, str(DELIVER), aout, host, str(port)],
                       capture_output=True, text=True, timeout=120)
    return r.stdout

def main():
    target = sys.argv[1] if len(sys.argv) > 1 else "qemu"
    host, port = TARGETS[target]
    tests = sorted(p.stem for p in (HERE / "corpus").glob("*.zig"))
    (HERE / "_out").mkdir(exist_ok=True)
    results, npass, ncf, nrf = [], 0, 0, 0
    for name in tests:
        cok, cerr, aout = build(name)
        if not cok:
            err1 = next((l for l in cerr.splitlines() if "error:" in l or "panic:" in l), cerr.strip()[:200])
            results.append({"name": name, "compile": False, "run": None, "detail": err1})
            ncf += 1; print(f"  COMPILE-FAIL  {name}  :: {err1}"); continue
        try:
            output = run_on(aout, host, port)
        except Exception as e:
            results.append({"name": name, "compile": True, "run": "error", "detail": str(e)})
            nrf += 1; print(f"  RUN-ERROR     {name}  :: {e}"); continue
        passed = (f"ok {name}" in output) and ("FAIL" not in output)
        results.append({"name": name, "compile": True, "run": passed,
                        "detail": output.strip().replace("\n", " | ")[:200]})
        if passed: npass += 1; print(f"  PASS          {name}")
        else: nrf += 1; print(f"  RUN-FAIL      {name}  :: {output.strip()[:120]!r}")

    manifest = {"target": target, "host": f"{host}:{port}", "zig_version": zig_version(),
                "compiler": "zig9-patched (container)", "optimize": "ReleaseSmall",
                "total": len(tests), "passed": npass, "compile_failed": ncf, "run_failed": nrf,
                "results": results}
    mdir = HERE / "parity/manifests"; mdir.mkdir(parents=True, exist_ok=True)
    (mdir / f"{target}.json").write_text(json.dumps(manifest, indent=2) + "\n")
    print(f"\n{target}: {npass}/{len(tests)} pass  ({ncf} compile-fail, {nrf} run-fail)"
          f"  -> test/parity/manifests/{target}.json")
    return 0 if npass == len(tests) else 1

if __name__ == "__main__":
    sys.exit(main())
