const std = @import("std");
fn w(s: []const u8) void { _ = std.os.plan9.write(2, s.ptr, s.len); }
var dbg: std.heap.DebugAllocator(.{ .stack_trace_frames = 0 }) = .init;
pub fn main() void {
    w("A: alloc stress start\n");
    const a = dbg.allocator();
    var list = std.ArrayList([]u8).init(a);
    var seed: u64 = 12345;
    var i: usize = 0;
    while (i < 200000) : (i += 1) {
        seed = seed *% 6364136223846793005 +% 1442695040888963407;
        const sz = (seed >> 33) % 4096 + 1;
        const buf = a.alloc(u8, sz) catch { w("alloc FAIL\n"); return; };
        @memset(buf, @truncate(i));
        list.append(buf) catch { w("append FAIL\n"); return; };
        // free some to exercise reuse
        if (list.items.len > 500) {
            const idx = (seed >> 20) % list.items.len;
            a.free(list.items[idx]);
            _ = list.swapRemove(idx);
        }
    }
    w("B: alloc stress survived 200k ops\n");
    // free the rest
    for (list.items) |b| a.free(b);
    list.deinit();
    w("C: freed all, ok\n");
}
