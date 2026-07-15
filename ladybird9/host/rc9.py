#!/usr/bin/env python3
"""
rc9.py HOST PORT SCRIPT — run an rc script on the box over listen1, print its
output. The listener eats bare '#' on the command line, so multi-line scripts
are shipped as a heredoc'd FILE and executed, which survives '#g'/'#s' paths.
"""
import sys, socket


def send(host, port, cmd, wait=20.0):
    s = socket.create_connection((host, port), timeout=6)
    s.sendall(cmd.encode())
    s.shutdown(socket.SHUT_WR)
    s.settimeout(wait)
    out = b''
    try:
        while True:
            b = s.recv(8192)
            if not b:
                break
            out += b
    except socket.timeout:
        pass
    s.close()
    return out.decode('latin-1')


def main():
    host, port, script = sys.argv[1], int(sys.argv[2]), sys.argv[3]
    # Ship to a file, then run — dodges the '#'-on-command-line filter.
    payload = "cat > /tmp/rc9.$pid <<'XEOF'\n" + script + "\nXEOF\nrc /tmp/rc9.$pid >[2=1]; rm -f /tmp/rc9.$pid\n"
    sys.stdout.write(send(host, port, payload))


if __name__ == '__main__':
    main()
