#!/usr/bin/env python3
"""ship.py AOUT [host] [port] [remote] — deliver a BIG Plan 9 a.out over listen1.

cc9/host/deliver.py emits the artifact as a C `uchar[]` and compiles it on the
box: ~6 bytes of C per byte shipped. A 13MB binary becomes a ~78MB source file,
which is not a transfer, it's a denial of service. The house alternative is
http+hget (see dosbox9/host/deploy.sh), but that means exposing a directory to
the LAN.

This ships through the listen1 channel we already have: gzip on the host, base64
so the text survives rc, chunked appends, then a small fixed-size decoder (b64d.c)
compiled once on the box, and gunzip. Transfer cost is ~1.33x the *compressed*
size instead of ~6x the uncompressed size.

  ship.py /tmp/jshello.aout 192.168.88.159 17010 /tmp/jshello
"""
import base64
import gzip
import socket
import sys
import os

CHUNK = 48 * 1024          # base64 chars per rc heredoc append
HERE = os.path.dirname(os.path.abspath(__file__))


def send(host, port, cmd, wait=20.0):
    s = socket.create_connection((host, port), timeout=10)
    s.sendall(cmd.encode())
    s.shutdown(socket.SHUT_WR)
    s.settimeout(wait)
    out = b''
    try:
        while True:
            b = s.recv(65536)
            if not b:
                break
            out += b
    except socket.timeout:
        pass
    s.close()
    return out.decode('latin-1')


# 9front's /tmp is a RAMFS. This script briefly holds THREE copies on the box
# (.b64 + .gz + the decoded output), so the peak cost is ~2.4x the binary. Pushing
# a 162MB servoshell through /tmp wedged cirno hard enough to need a power cycle
# (2026-07-15) — hence the guard and the staging-dir option below.
MAX_TMP_BYTES = 24 * 1024 * 1024  # refuse >24MB into a ramfs path


def looks_like_ramfs(path):
    """True for paths that are RAM on 9front. /tmp is the big one."""
    return path == '/tmp' or path.startswith('/tmp/')


def main():
    aout = sys.argv[1]
    host = sys.argv[2] if len(sys.argv) > 2 else '127.0.0.1'
    port = int(sys.argv[3]) if len(sys.argv) > 3 else 1717
    remote = sys.argv[4] if len(sys.argv) > 4 else '/tmp/shipped'
    # Staging (.b64/.gz) defaults next to the destination, so a disk-backed
    # `remote` keeps the whole operation off the ramfs.
    stage = os.path.dirname(remote) or '/tmp'

    raw = open(aout, 'rb').read()

    if looks_like_ramfs(remote) or looks_like_ramfs(stage):
        peak = int(len(raw) * 2.4)
        if len(raw) > MAX_TMP_BYTES:
            print(
                f'REFUSING: {aout} is {len(raw)/1048576:.0f}MB and the destination\n'
                f'  ({remote}) is under /tmp, which is a RAMFS on 9front. This would\n'
                f'  hold ~{peak/1048576:.0f}MB in RAM at once (b64 + gz + output) and can\n'
                f'  wedge the box hard enough to need a power cycle.\n'
                f'  Ship to a disk-backed path instead, e.g.:\n'
                f'    ship.py {aout} {host} {port} /usr/glenda/servo\n'
                f'  and strip the binary first.',
                file=sys.stderr)
            return 2

    gz = gzip.compress(raw, 9)
    b64 = base64.b64encode(gz).decode('ascii')
    print(f'{aout}: {len(raw)} -> gz {len(gz)} -> b64 {len(b64)} '
          f'({len(b64)/len(raw):.2f}x of raw); staging in {stage}', file=sys.stderr)

    # The decoder is tiny and fixed-size, so deliver.py's own trick is fine here.
    dec = open(os.path.join(HERE, 'b64d.c')).read()
    send(host, port, "cat > /tmp/b64d.c <<'XEOF'\n" + dec + "\nXEOF\necho W\n")
    r = send(host, port, 'cd /tmp; 6c b64d.c && 6l -o b64d b64d.6 && echo BUILT\n', 90)
    if 'BUILT' not in r:
        print('b64d build failed:\n' + r, file=sys.stderr)
        return 1

    send(host, port, f'rm -f {stage}/ship.b64 {stage}/ship.gz {remote}\n', 15)
    n = (len(b64) + CHUNK - 1) // CHUNK
    for i in range(n):
        part = b64[i * CHUNK:(i + 1) * CHUNK]
        # Append, don't truncate: each chunk is its own rc invocation.
        send(host, port, f"cat >> {stage}/ship.b64 <<'XEOF'\n" + part + "\nXEOF\n", 30)
        print(f'\r  chunk {i+1}/{n}', end='', file=sys.stderr, flush=True)
    print(file=sys.stderr)

    r = send(host, port, f'ls -l {stage}/ship.b64\n', 15)
    print('  remote b64: ' + r.strip(), file=sys.stderr)

    r = send(host, port, f'cd /tmp; ./b64d {stage}/ship.b64 {stage}/ship.gz && echo DECODED\n', 300)
    if 'DECODED' not in r:
        print('decode failed:\n' + r, file=sys.stderr)
        return 1
    # Drop the .b64 before gunzip: it is the largest intermediate and is dead
    # once decoded. Holding it through gunzip is what makes the peak 2.4x.
    send(host, port, f'rm -f {stage}/ship.b64\n', 30)
    r = send(host, port, f'gunzip < {stage}/ship.gz > {remote} && chmod +x {remote} && echo GUNZIPPED\n', 600)
    if 'GUNZIPPED' not in r:
        print('gunzip failed:\n' + r, file=sys.stderr)
        return 1
    send(host, port, f'rm -f {stage}/ship.gz\n', 30)
    print('  ' + send(host, port, f'ls -l {remote}\n', 15).strip(), file=sys.stderr)
    return 0


if __name__ == '__main__':
    sys.exit(main())
