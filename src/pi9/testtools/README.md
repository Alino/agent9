# pi9 testtools

Diagnostic + verification helpers for pi9 development.

## celldump.c

Plan9 C utility that reads one frame from a vts session's `/n/vts/<s>/cells`
binary diff stream and prints each cell as `(row, col) char fg=N bg=N a=N`.

Use it to verify pi9's rendered output AT THE CELL LEVEL — independent
of whether vtwin (the libdraw client) is running. Useful for headless
testing and CI-style verification.

### Build inside the VM

```
6c -FTVw -o celldump.6 celldump.c
6l -o celldump celldump.6
```

### Usage

```
/tmp/celldump                  # default: /n/vts/1/cells
/tmp/celldump /n/vts/2/cells   # specific session
```

### Wire format reference

See `wiki/concepts/vt-9p-namespace.md` § "Wire format".

## Workflow used in Phase 1

Mac side:
1. Build `pi9.plan9.amd64` and `celldump.c`
2. Serve via `python3 -m http.server 8765 --directory src/pi9`

VM side (via `nc -w 30 127.0.0.1 1717`):
```
hget http://10.0.2.2:8765/pi9.plan9.amd64 > /tmp/pi9
hget http://10.0.2.2:8765/celldump.c > /tmp/celldump.c
chmod +x /tmp/pi9
6c -FTVw -o /tmp/celldump.6 /tmp/celldump.c
6l -o /tmp/celldump /tmp/celldump.6
mount /srv/vts /n/vts
echo '/tmp/pi9' > /n/vts/1/cons
sleep 2
/tmp/celldump /n/vts/1/cells > /tmp/cells.out
echo q > /n/vts/1/cons
```

Read `/tmp/cells.out` back to verify pi9 emitted the expected characters
with expected colors.

## diag scripts (transient)

`diag*.rc` files used during Phase 1 debugging are not kept. They were
ad-hoc rc scripts that injected commands into vts session 1 and dumped
results. The pattern they follow is reusable; if you need it again,
write a fresh diag script with `mount /srv/vts /n/vts; slay pi9;
echo /tmp/pi9 > /n/vts/1/cons; sleep N; /tmp/celldump ...`.
