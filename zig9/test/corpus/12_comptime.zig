const std = @import("std");
const p = std.os.plan9;
fn out(s: []const u8) void { _ = p.write(1, s.ptr, s.len); }
fn Vec(comptime n: usize, comptime T: type) type {
    return struct { d: [n]T,
        fn dot(a: @This(), b: @This()) T { var s: T = 0; inline for (0..n) |i| s += a.d[i] * b.d[i]; return s; } };
}
const squares = blk: { var a: [8]u32 = undefined; for (&a, 0..) |*e, i| e.* = @intCast(i * i); break :blk a; };
pub fn main() void {
    const V3 = Vec(3, i32);
    const r = (V3{ .d = .{ 1, 2, 3 } }).dot(.{ .d = .{ 4, 5, 6 } }); // 32
    if (r != 32) return out("FAIL 12_comptime dot\n");
    if (squares[7] != 49) return out("FAIL 12_comptime table\n");
    out("ok 12_comptime\n");
}
