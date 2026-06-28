const std = @import("std");
const p = std.os.plan9;
fn out(s: []const u8) void { _ = p.write(1, s.ptr, s.len); }
const Shape = union(enum) { circle: f64, square: f64, fn area(s: Shape) f64 {
    return switch (s) { .circle => |r| 3.14159 * r * r, .square => |a| a * a }; } };
pub fn main() void {
    const c = Shape{ .circle = 2.0 };
    const sq = Shape{ .square = 3.0 };
    if (sq.area() != 9.0) return out("FAIL 07_enums\n");
    if (!(c.area() > 12.56 and c.area() < 12.57)) return out("FAIL 07_enums circle\n");
    out("ok 07_enums\n");
}
