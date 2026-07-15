# tailscale9 — Tailscale on 9front

Upstream Tailscale (GOOS=plan9 is an official port since 2025) cross-compiled
for plan9/amd64 with stock Go. Userspace WireGuard via gVisor's netstack — no
kernel modifications, runs on stock 9front.

## Install

    pac9 install tailscale9

## Use

    tailscaled &          # rc wrapper: sets up loopback + statedir, execs the daemon
    tailscale up          # prints a login URL — open it anywhere to authenticate
    tailscale status

State lives in `$home/lib/tailscale`. The wrapper exists because tailscaled's
control socket listens on 127.0.0.1:5252 and a fresh 9front has no loopback
interface (`ip/ipconfig loopback /dev/null 127.1`).

Upstream notes: tailscale.com/blog/plan9-port (their binaries are 386/9legacy;
this package is amd64 built for 9front — verified on bare metal).

## Release

    release/make-tarball.sh   # cross-builds with host Go, stages, tars
