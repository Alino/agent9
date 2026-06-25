#!/usr/bin/env python3
"""
deliver.py AOUT [host] [port] — deliver a Plan 9 a.out to cirno over listen1
and run it. listen1 mangles raw binary, so we ship a generated C byte-writer
(it dumps the host-computed bytes to a file), compile it on the box, run it to
materialize the binary, then exec it and stream the output.
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
    data = open(aout, 'rb').read()
    arr = ', '.join(f'0x{b:02x}' for b in data)
    writer = (f'#include <u.h>\n#include <libc.h>\nuchar data[] = {{{arr}}};\n'
              f'void main(void){{int fd=create("/tmp/cc9bin",OWRITE,0755);'
              f'write(fd,data,sizeof data);close(fd);exits(nil);}}\n')
    send(host, port, "cat > /tmp/cc9mk.c <<'XEOF'\n" + writer + "XEOF\necho W\n")
    send(host, port, "cd /tmp; 6c cc9mk.c\n", 60)
    send(host, port, "cd /tmp; 6l -o cc9mk cc9mk.6\n", 60)
    send(host, port, "/tmp/cc9mk >/dev/null >[2=1]\n", 20)
    out = send(host, port, "/tmp/cc9bin\n", 20)
    sys.stdout.write(out)

if __name__ == '__main__':
    main()
