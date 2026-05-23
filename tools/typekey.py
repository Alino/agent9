#!/usr/bin/env python3
# typekey.py — send a string to QEMU as keypresses via HMP sendkey
import socket, sys, time

QMP_SOCK = "/tmp/qemu-9front.sock"

# QEMU keynames for visible characters
KEYS = {
    " ": "spc", ".": "dot", "-": "minus", "/": "slash", ";": "semicolon",
    ",": "comma", "=": "equal", "'": "apostrophe", "`": "grave_accent",
    "[": "bracket_left", "]": "bracket_right", "\\": "backslash",
    "\n": "ret", "\t": "tab",
}
for c in "abcdefghijklmnopqrstuvwxyz":
    KEYS[c] = c
for c in "0123456789":
    KEYS[c] = c

SHIFTED = {
    "!": "shift-1", "@": "shift-2", "#": "shift-3", "$": "shift-4",
    "%": "shift-5", "^": "shift-6", "&": "shift-7", "*": "shift-8",
    "(": "shift-9", ")": "shift-0", "_": "shift-minus", "+": "shift-equal",
    "{": "shift-bracket_left", "}": "shift-bracket_right",
    "|": "shift-backslash", ":": "shift-semicolon", '"': "shift-apostrophe",
    "<": "shift-comma", ">": "shift-dot", "?": "shift-slash",
    "~": "shift-grave_accent",
}

def hmp(cmd):
    s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    s.connect(QMP_SOCK)
    time.sleep(0.05)
    s.recv(8192)
    s.send((cmd + "\n").encode())
    time.sleep(0.04)
    try:
        s.recv(8192)
    except: pass
    s.close()

def keyfor(c):
    if c in KEYS: return KEYS[c]
    if c in SHIFTED: return SHIFTED[c]
    if c.isupper(): return "shift-" + c.lower()
    return None

def main():
    text = sys.argv[1] if len(sys.argv) > 1 else sys.stdin.read()
    for c in text:
        k = keyfor(c)
        if not k:
            print(f"skip: {c!r}", file=sys.stderr)
            continue
        hmp(f"sendkey {k}")

if __name__ == "__main__":
    main()
