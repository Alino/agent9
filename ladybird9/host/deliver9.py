#!/usr/bin/env python3
"""
deliver9.py AOUT [host] [port] [name] — cc9/host/deliver.py with UNIQUE on-box
scratch names (default 'lb9'): /tmp/<name>mk.c, /tmp/<name>mk, /tmp/<name>bin.
Multiple agent sessions share the dev VM and the stock deliver.py's fixed
/tmp/cc9bin races between them (a gate ran here and printed another session's
binary output). Runs nothing: use run9.py or a detached rc line to execute.
"""
import sys, socket

def send(host, port, cmd, wait=12.0):
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
    aout = sys.argv[1]
    host = sys.argv[2] if len(sys.argv) > 2 else '127.0.0.1'
    port = int(sys.argv[3]) if len(sys.argv) > 3 else 1717
    name = sys.argv[4] if len(sys.argv) > 4 else 'lb9'
    data = open(aout, 'rb').read()
    arr = ', '.join(f'0x{b:02x}' for b in data)
    writer = (f'#include <u.h>\n#include <libc.h>\nuchar data[] = {{{arr}}};\n'
              f'void main(void){{int fd=create("/tmp/{name}bin",OWRITE,0755);'
              f'write(fd,data,sizeof data);close(fd);exits(nil);}}\n')
    send(host, port, f"cat > /tmp/{name}mk.c <<'XEOF'\n" + writer + "XEOF\necho W\n")
    send(host, port, f"cd /tmp; 6c {name}mk.c\n", 90)
    send(host, port, f"cd /tmp; 6l -o {name}mk {name}mk.6\n", 90)
    send(host, port, f"/tmp/{name}mk >/dev/null >[2=1]; ls -l /tmp/{name}bin\n", 30)
    print(f"delivered /tmp/{name}bin", file=sys.stderr)

if __name__ == '__main__':
    main()
