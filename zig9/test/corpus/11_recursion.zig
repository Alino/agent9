const std = @import("std");
const p = std.os.plan9;
fn out(s: []const u8) void { _ = p.write(1, s.ptr, s.len); }
fn fib(n: u64) u64 { return if (n < 2) n else fib(n - 1) + fib(n - 2); }
pub fn main() void {
    if (fib(20) != 6765) return out("FAIL 11_recursion\n");
    out("ok 11_recursion\n");
}
