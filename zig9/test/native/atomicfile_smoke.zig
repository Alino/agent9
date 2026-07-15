const std = @import("std");
fn w(s: []const u8) void { _ = std.os.plan9.write(2, s.ptr, s.len); }
pub fn main() void {
    w("A: getrandom\n");
    var rb: [16]u8 = undefined;
    std.posix.getrandom(&rb) catch { w("getrandom FAIL\n"); return; };
    w("B: crypto.random.bytes\n");
    var rb2: [12]u8 = undefined;
    std.crypto.random.bytes(&rb2);
    w("C: dir.atomicFile write+finish\n");
    var dir = std.fs.cwd();
    dir.makePath("zig9af") catch { w("makePath FAIL\n"); return; };
    var af = dir.atomicFile("zig9af/builtin.zig", .{ .make_path = true }) catch { w("atomicFile FAIL\n"); return; };
    defer af.deinit();
    af.file.writeAll("pub const target = 1;\n") catch { w("writeAll FAIL\n"); return; };
    af.finish() catch |e| { w("finish FAIL: "); w(@errorName(e)); w(" errstr="); w(std.os.plan9.errstr()); w("\n"); return; };
    const st = dir.statFile("zig9af/builtin.zig") catch { w("stat FAIL\n"); return; };
    var ob: [48]u8 = undefined;
    const so = std.fmt.bufPrint(&ob, "D: ok atomicFile, size={d}\n", .{st.size}) catch return;
    w(so);
}
