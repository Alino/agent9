#!/usr/bin/env python3
"""qmp.py — drive the 9front dev VM over the qemu HMP monitor socket:
  qmp.py type 'text...'      (\\n for Enter; typed slowly — gl9win2 drops fast keys)
  qmp.py key ret|esc|ctrl-c  (raw sendkey arg)
  qmp.py shot out.ppm        (screendump to host path)
"""
import socket, sys, time

SOCK = '/tmp/qemu-9front.sock'

KEY = {}
for c in 'abcdefghijklmnopqrstuvwxyz0123456789':
    KEY[c] = c
for i, c in enumerate(')!@#$%^&*('):
    KEY[c] = 'shift-' + str(i)
for c in 'ABCDEFGHIJKLMNOPQRSTUVWXYZ':
    KEY[c] = 'shift-' + c.lower()
KEY.update({
    ' ': 'spc', '\n': 'ret', '\t': 'tab',
    '-': 'minus', '_': 'shift-minus', '=': 'equal', '+': 'shift-equal',
    '[': 'bracket_left', '{': 'shift-bracket_left',
    ']': 'bracket_right', '}': 'shift-bracket_right',
    ';': 'semicolon', ':': 'shift-semicolon',
    "'": 'apostrophe', '"': 'shift-apostrophe',
    ',': 'comma', '<': 'shift-comma', '.': 'dot', '>': 'shift-dot',
    '/': 'slash', '?': 'shift-slash', '\\': 'backslash', '|': 'shift-backslash',
    '`': 'grave_accent', '~': 'shift-grave_accent',
})

def main():
    s = socket.socket(socket.AF_UNIX)
    s.connect(SOCK)
    s.settimeout(3)
    time.sleep(0.2)
    s.recv(4096)                       # banner + prompt
    def cmd(c):
        s.sendall((c + '\n').encode())
        time.sleep(0.05)
        try:
            s.recv(4096)
        except socket.timeout:
            pass
    mode, arg = sys.argv[1], sys.argv[2]
    if mode == 'type':
        for ch in arg.replace('\\n', '\n'):
            cmd('sendkey ' + KEY[ch])
            time.sleep(0.1)
    elif mode == 'key':
        cmd('sendkey ' + arg)
    elif mode == 'shot':
        cmd('screendump ' + arg)
        time.sleep(0.5)
    print('ok')

if __name__ == '__main__':
    main()
