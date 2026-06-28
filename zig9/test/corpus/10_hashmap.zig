const std = @import("std");
const p = std.os.plan9;
fn out(s: []const u8) void { _ = p.write(1, s.ptr, s.len); }
pub fn main() void {
    const a = std.heap.page_allocator;
    var m = std.AutoHashMap(u32, u32).init(a);
    defer m.deinit();
    var i: u32 = 0; while (i < 50) : (i += 1) m.put(i, i * 3) catch return out("FAIL 10_hashmap put\n");
    if ((m.get(7) orelse 0) != 21 or m.count() != 50) return out("FAIL 10_hashmap\n");
    out("ok 10_hashmap\n");
}
