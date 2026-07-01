#!/usr/bin/env python3
# run_gl9.py [NAME...] — the gl9 parity runner. For each corpus program: link it
# for 9front (cc9), serve it over HTTP, hget it onto the qemu VM, run it with
# GALLIUM_NOSSE=1 (softpipe's x86 JIT NX-faults on 9front — this forces the C
# path), fetch the P3 PPM back, and diff vs the host Mesa softpipe golden. Writes
# test/parity/manifests/qemu.json.
#
# The listen1 control channel (nc) is flaky and the softpipe run is slow, so every
# VM step retries and long ops run in the background on the VM against a file we poll.
import json, os, subprocess, sys, time

GL9 = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
CORPUS = os.path.join(GL9, "test", "corpus")
GOLD = os.path.join(GL9, "test", "goldens")
CAP = os.path.join(GL9, "test", "_captures")
OUT = os.path.join(GL9, "_out")
MANIFEST = os.path.join(GL9, "test", "parity", "manifests", "qemu.json")

VM_HOST, VM_PORT = "127.0.0.1", "1717"       # listen1 (host-forwarded)
VM_GW = "10.0.2.2"                            # host, as seen from the guest
HTTP_PORT = 8099
W = H = 64                                    # must match glharness GL9_W/H
NPIX = W * H
ALL = ["01_clear_color", "02_triangle", "03_shaded_triangle",
       "04_textured_quad", "05_instanced_quads", "06_depth_blend"]


def vmrc(cmd, wait=12):
    try:
        p = subprocess.run(["nc", "-w", str(wait), VM_HOST, VM_PORT],
                           input=cmd + "\n", capture_output=True, text=True,
                           timeout=wait + 6)
        return p.stdout
    except subprocess.TimeoutExpired:
        return ""


def vmrc_retry(cmd, wait=12, tries=4):
    for _ in range(tries):
        o = vmrc(cmd, wait)
        if o.strip():
            return o
        time.sleep(1)
    return ""


def serve():
    up = subprocess.run(["bash", "-c", f"lsof -nP -iTCP:{HTTP_PORT} | grep -q LISTEN"]).returncode == 0
    if not up:
        subprocess.Popen(["bash", "-c",
            f"cd {OUT} && nohup python3 -m http.server {HTTP_PORT} --bind 0.0.0.0 "
            f">/tmp/gl9http.log 2>&1 &"])
        time.sleep(1)


def link(name):
    subprocess.run(["python3", os.path.join(GL9, "host", "build-gl9.py"),
                    "link", os.path.join(CORPUS, name + ".c")],
                   check=True, capture_output=True, text=True)
    return os.path.join(OUT, name + ".aout")


def transfer(name, size):
    vmrc(f"rm -f /tmp/{name}.aout /tmp/{name}.ppm /tmp/{name}.log")
    for _ in range(8):
        vmrc(f"hget http://{VM_GW}:{HTTP_PORT}/{name}.aout >/tmp/{name}.aout >[2]/dev/null &", 8)
        time.sleep(1)
        s = vmrc(f"ls -l /tmp/{name}.aout | awk '{{print $6}}'", 8).strip()
        if s and s != "0":
            break
    for _ in range(80):
        s = vmrc(f"ls -l /tmp/{name}.aout | awk '{{print $6}}'", 8).strip()
        if s == str(size):
            return True
        time.sleep(3)
    return False


def run(name):
    vmrc(f"chmod +x /tmp/{name}.aout; GALLIUM_NOSSE=1; "
         f"/tmp/{name}.aout /tmp/{name}.ppm >/tmp/{name}.log >[2=1] &", 8)
    for _ in range(48):
        log = vmrc(f"cat /tmp/{name}.log >[2]/dev/null", 8)
        if "SIG" in log:
            return "ok"
        if "fault" in log or "suicide" in log:
            print(f"  {name}: FAULT: {log.strip().splitlines()[-1] if log.strip() else '?'}")
            return "fault"
        time.sleep(5)
    return "timeout"


def fetch_ppm(name):
    os.makedirs(CAP, exist_ok=True)
    dst = os.path.join(CAP, name + ".ppm")
    for _ in range(8):
        out = vmrc(f"cat /tmp/{name}.ppm", 30)
        toks = out.split()
        if len(toks) >= 4 + NPIX * 3 and toks[0] == "P3":
            open(dst, "w").write(out)
            return dst
        time.sleep(2)
    return None


def diff(name, cap):
    gold = os.path.join(GOLD, name + ".ppm")
    p = subprocess.run(["python3", os.path.join(GL9, "test", "ppmdiff.py"),
                        gold, cap, "--tol", "2", "--pass", "99.5"],
                       capture_output=True, text=True)
    line = p.stdout.strip()
    pct = 0.0
    for t in line.split():
        if t.endswith("%"):
            try: pct = float(t[:-1])
            except ValueError: pass
    return (p.returncode == 0), pct, line


def main():
    names = sys.argv[1:] or ALL
    serve()
    results = []
    for name in names:
        print(f"== {name} ==")
        aout = link(name)
        size = os.path.getsize(aout)
        r = {"name": name, "size": size}
        if not transfer(name, size):
            r["status"] = "transfer_failed"; results.append(r); print("  transfer failed"); continue
        st = run(name)
        if st != "ok":
            r["status"] = st; results.append(r); continue
        cap = fetch_ppm(name)
        if not cap:
            r["status"] = "ppm_fetch_failed"; results.append(r); print("  ppm fetch failed"); continue
        ok, pct, line = diff(name, cap)
        r.update(status="pass" if ok else "fail", pass_pct=pct, detail=line)
        results.append(r)
        print("  " + line)

    npass = sum(1 for r in results if r.get("status") == "pass")
    manifest = {"target": "qemu", "host": "mesa-24.0.9-softpipe",
                "total": len(results), "pass": npass, "results": results}
    os.makedirs(os.path.dirname(MANIFEST), exist_ok=True)
    json.dump(manifest, open(MANIFEST, "w"), indent=1)
    print(f"\n{npass}/{len(results)} pass -> {MANIFEST}")


if __name__ == "__main__":
    main()
