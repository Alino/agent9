const std = @import("std");
pub fn main() !void {
    const w = std.io.getStdOut().writer();
    try w.print("S1 stdio ok\n", .{});
    const a = std.heap.page_allocator;
    const buf = try a.alloc(u8, 100_000);
    try w.print("S2 page_allocator alloc ok p=0x{x}\n", .{@intFromPtr(buf.ptr)});
    @memset(buf, 0xAB);
    try w.print("S3 memset ok\n", .{});
    var gpa = std.heap.DebugAllocator(.{}){};
    const g = gpa.allocator();
    var list = std.ArrayList(u64).init(g);
    var i: u64 = 0;
    while (i < 50_000) : (i += 1) try list.append(i * 7);
    try w.print("S4 debugalloc list ok sum-probe={d}\n", .{list.items[49_999]});
    list.deinit();
    a.free(buf);
    try w.print("S5 ALL OK\n", .{});
}
