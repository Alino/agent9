//! Minimal Zig test runner for 9front (zig9).
//!
//! The default test runner (lib/compiler/test_runner.zig) pulls in std.io, the
//! panic machinery and @errorName's error-name table — the last hits a gap in
//! the self-hosted x86_64 backend's plan9 lazy-symbol path. This runner avoids
//! all of that: a `void` main, raw plan9 write syscalls, and @intFromError
//! instead of @errorName. It iterates builtin.test_functions, runs each, counts
//! pass/skip/fail, prints a summary, and exits non-zero on any failure.
//!
//! Use with:  zig test FILE --test-runner test/plan9_test_runner.zig \
//!              -target x86_64-plan9 -mcpu=x86_64_v2 -OReleaseSmall \
//!              --test-no-exec -femit-bin=out.aout
//!
//! Caveat: a test that *panics* (rather than returning an error) aborts the run,
//! since a void-main runner can't install a recoverable panic handler here.
const builtin = @import("builtin");
const std = @import("std");
const p = std.os.plan9;

fn out(s: []const u8) void {
    _ = p.write(1, s.ptr, s.len);
}

fn num(n: u64) void {
    var b: [20]u8 = undefined;
    var i: usize = b.len;
    var x = n;
    if (x == 0) return out("0");
    while (x > 0) : (x /= 10) {
        i -= 1;
        b[i] = @intCast('0' + x % 10);
    }
    out(b[i..]);
}

pub fn main() void {
    var passed: u64 = 0;
    var skipped: u64 = 0;
    var failed: u64 = 0;
    for (builtin.test_functions) |t| {
        if (t.func()) |_| {
            passed += 1;
        } else |err| {
            if (err == error.SkipZigTest) {
                skipped += 1;
            } else {
                failed += 1;
                out("FAIL ");
                out(t.name);
                out(" (err ");
                num(@intFromError(err));
                out(")\n");
            }
        }
    }
    out("SUMMARY pass=");
    num(passed);
    out(" fail=");
    num(failed);
    out(" skip=");
    num(skipped);
    out("\n");
    if (failed > 0) p.exits("fail") else p.exits(null);
}
