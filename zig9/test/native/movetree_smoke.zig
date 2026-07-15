const std = @import("std");
const p9 = std.os.plan9;
fn w(s: []const u8) void { _ = p9.write(2, s.ptr, s.len); }
var dbg: std.heap.DebugAllocator(.{ .stack_trace_frames = 0 }) = .init;
fn rmTreeRec(parent: std.fs.Dir, name: []const u8) void {
    var d = parent.openDir(name, .{ .iterate = true }) catch {
        parent.deleteFile(name) catch {};
        return;
    };
    var it = d.iterate();
    while (it.next() catch null) |e| {
        if (e.kind == .directory) rmTreeRec(d, e.name) else d.deleteFile(e.name) catch {};
    }
    d.close();
    parent.deleteDir(name) catch {};
}
fn copyDirRec(gpa: std.mem.Allocator, sdir: std.fs.Dir, spath: []const u8, ddir: std.fs.Dir, dpath: []const u8) !void {
    try ddir.makePath(dpath);
    var s = try sdir.openDir(spath, .{ .iterate = true });
    defer s.close();
    var it = s.iterate();
    while (try it.next()) |e| {
        var b1: [std.fs.max_path_bytes]u8 = undefined;
        var b2: [std.fs.max_path_bytes]u8 = undefined;
        const sp = try std.fmt.bufPrint(&b1, "{s}/{s}", .{ spath, e.name });
        const dp = try std.fmt.bufPrint(&b2, "{s}/{s}", .{ dpath, e.name });
        switch (e.kind) {
            .directory => try copyDirRec(gpa, sdir, sp, ddir, dp),
            .file => try sdir.copyFile(sp, ddir, dp, .{}),
            else => {},
        }
    }
}
fn moveTree(gpa: std.mem.Allocator, dir: std.fs.Dir, src: []const u8, dst: []const u8) !void {
    rmTreeRec(dir, dst);
    try copyDirRec(gpa, dir, src, dir, dst);
    rmTreeRec(dir, src);
}
pub fn main() void {
    const a = dbg.allocator();
    var dir = std.fs.cwd();
    dir.makePath("zig9mt/tmp/x/y") catch { w("mp FAIL\n"); return; };
    { var f = dir.createFile("zig9mt/tmp/x/f1.o", .{ .truncate = true }) catch { w("cf1\n"); return; }; f.writeAll("hello") catch {}; f.close(); }
    { var f = dir.createFile("zig9mt/tmp/x/y/f2.o", .{ .truncate = true }) catch { w("cf2\n"); return; }; f.writeAll("world!!") catch {}; f.close(); }
    dir.makePath("zig9mt/o") catch {};
    var mdir = dir.openDir("zig9mt", .{}) catch { w("od FAIL\n"); return; };
    defer mdir.close();
    moveTree(a, mdir, "tmp/x", "o/deadbeef") catch |e| { w("moveTree FAIL: "); w(@errorName(e)); w("\n"); return; };
    const s1 = mdir.statFile("o/deadbeef/f1.o") catch { w("stat f1 FAIL\n"); return; };
    const s2 = mdir.statFile("o/deadbeef/y/f2.o") catch { w("stat f2 FAIL\n"); return; };
    if (mdir.access("tmp/x", .{})) |_| { w("WARN: src not deleted\n"); } else |_| {}
    var ob: [64]u8 = undefined;
    const so = std.fmt.bufPrint(&ob, "ok moveTree f1={d} f2={d}\n", .{ s1.size, s2.size }) catch return;
    w(so);
}
