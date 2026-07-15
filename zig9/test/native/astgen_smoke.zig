const std = @import("std");
fn w(s: []const u8) void { _ = std.os.plan9.write(2, s.ptr, s.len); }
var dbg: std.heap.DebugAllocator(.{ .stack_trace_frames = 0 }) = .init;
pub fn main() void {
    const a = dbg.allocator();
    w("A: read source\n");
    const src = std.fs.cwd().readFileAllocOptions(a, "/sys/lib/zig9/lib/std/mem.zig", 8 * 1024 * 1024, null, @alignOf(u8), 0) catch { w("read FAIL\n"); return; };
    w("B: parse\n");
    var ast = std.zig.Ast.parse(a, src, .zig) catch { w("parse FAIL\n"); return; };
    defer ast.deinit(a);
    if (ast.errors.len != 0) { w("C: parse errors present\n"); }
    w("D: astgen (ZIR)\n");
    var zir = std.zig.AstGen.generate(a, ast) catch { w("astgen FAIL\n"); return; };
    defer zir.deinit(a);
    w("E: astgen OK\n");
    var buf: [64]u8 = undefined;
    const s = std.fmt.bufPrint(&buf, "F: zir instructions={d}\n", .{zir.instructions.len}) catch return;
    w(s);
}
