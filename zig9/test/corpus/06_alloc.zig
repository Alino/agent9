const std = @import("std");
const p = std.os.plan9;
fn out(s: []const u8) void { _ = p.write(1, s.ptr, s.len); }
pub fn main() void {
    const a = std.heap.page_allocator;
    var list = std.ArrayList(u32).init(a);
    defer list.deinit();
    var i: u32 = 0; while (i < 100) : (i += 1) list.append(i * i) catch return out("FAIL 06_alloc append\n");
    if (list.items.len != 100 or list.items[99] != 9801) return out("FAIL 06_alloc\n");
    out("ok 06_alloc\n");
}
