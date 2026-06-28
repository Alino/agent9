// Minimal zig9 hello for 9front. Uses a `void` main + a raw plan9 write syscall:
// the default `!void` + std.io path currently hits a self-hosted-backend limit
// on plan9 (named/lazy extern symbols) — see port/plan9/README.md. Build/run:
//   zig9/host/zig9 run zig9/test/hello.zig
const plan9 = @import("std").os.plan9;

pub fn main() void {
    const msg = "hello from zig9 on 9front\n";
    _ = plan9.write(1, msg.ptr, msg.len);
    plan9.exits(null);
}
