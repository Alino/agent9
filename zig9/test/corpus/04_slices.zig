const std = @import("std");
const p = std.os.plan9;
fn out(s: []const u8) void { _ = p.write(1, s.ptr, s.len); }
pub fn main() void {
    var data = [_]u32{ 5, 3, 9, 1, 7, 2, 8 };
    std.mem.sort(u32, &data, {}, std.sort.asc(u32));
    if (data[0] != 1 or data[data.len - 1] != 9) return out("FAIL 04_slices sort\n");
    var sum: u32 = 0;
    for (data[1..4]) |x| sum += x; // 2+3+5=10
    if (sum != 10) return out("FAIL 04_slices slice\n");
    out("ok 04_slices\n");
}
