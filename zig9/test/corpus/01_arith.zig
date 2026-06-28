const std = @import("std");
const p = std.os.plan9;
fn out(s: []const u8) void { _ = p.write(1, s.ptr, s.len); }
pub fn main() void {
    var a: u64 = 7; const b: u64 = 6;
    a = a * b + 1; // 43
    var buf: [64]u8 = undefined;
    if (a != 43) return out("FAIL 01_arith\n");
    const s = std.fmt.bufPrint(&buf, "ok 01_arith ({d})\n", .{a}) catch return out("FAIL 01_arith fmt\n");
    out(s);
}
