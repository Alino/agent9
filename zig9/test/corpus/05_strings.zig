const std = @import("std");
const p = std.os.plan9;
fn out(s: []const u8) void { _ = p.write(1, s.ptr, s.len); }
pub fn main() void {
    const h = "plan9 forever";
    if (std.mem.indexOf(u8, h, "forever") != 6) return out("FAIL 05_strings idx\n");
    if (!std.mem.eql(u8, h[0..5], "plan9")) return out("FAIL 05_strings eql\n");
    var it = std.mem.splitScalar(u8, "a,b,c", ',');
    var n: usize = 0; while (it.next()) |_| n += 1;
    if (n != 3) return out("FAIL 05_strings split\n");
    out("ok 05_strings\n");
}
