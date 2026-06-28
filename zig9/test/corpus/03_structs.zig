const std = @import("std");
const p = std.os.plan9;
fn out(s: []const u8) void { _ = p.write(1, s.ptr, s.len); }
const Pt = struct { x: i32, y: i32, fn sum(self: Pt) i32 { return self.x + self.y; } };
var g: [16]Pt = undefined;
pub fn main() void {
    for (&g, 0..) |*e, i| e.* = .{ .x = @intCast(i), .y = @intCast(i * 2) };
    var total: i32 = 0;
    for (g) |e| total += e.sum();
    if (total != 360) return out("FAIL 03_structs\n"); // sum i + 2i for i in 0..15 = 3*120
    out("ok 03_structs\n");
}
