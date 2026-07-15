const std = @import("std");
fn w(s: []const u8) void { _ = std.os.plan9.write(2, s.ptr, s.len); }
pub fn main() void {
    var dir = std.fs.cwd();
    dir.makePath("zig9cache/z") catch { w("makePath FAIL\n"); return; };
    var zdir = dir.openDir("zig9cache/z", .{}) catch { w("openDir FAIL\n"); return; };
    defer zdir.close();
    w("A: createFile w/ lock=.shared truncate=false read=true\n");
    var f = zdir.createFile("deadbeefcafe0123", .{ .read = true, .truncate = false, .lock = .shared }) catch { w("createFile FAIL\n"); return; };
    defer f.close();
    w("B: setEndPos(0) [ftruncate/fwstat]\n");
    f.setEndPos(0) catch { w("setEndPos FAIL\n"); return; };
    w("C: seekTo(0) [lseek]\n");
    f.seekTo(0) catch { w("seekTo FAIL\n"); return; };
    w("D: writevAll (5 iovecs like saveZirCache)\n");
    const hdr = [_]u8{ 1, 2, 3, 4, 5, 6, 7, 8 };
    const tags = [_]u8{ 0xAA, 0xBB, 0xCC };
    const data = [_]u8{ 0x11, 0x22, 0x33, 0x44, 0x55, 0x66 };
    const s1 = [_]u8{ 'x', 'y' };
    const s2 = [_]u8{ 'z' };
    var iovecs = [_]std.posix.iovec_const{
        .{ .base = &hdr, .len = hdr.len },
        .{ .base = &tags, .len = tags.len },
        .{ .base = &data, .len = data.len },
        .{ .base = &s1, .len = s1.len },
        .{ .base = &s2, .len = s2.len },
    };
    f.writevAll(&iovecs) catch { w("writevAll FAIL\n"); return; };
    const expect_len = hdr.len + tags.len + data.len + s1.len + s2.len;
    w("E: seekTo(0) + read back\n");
    f.seekTo(0) catch { w("seek2 FAIL\n"); return; };
    var buf: [64]u8 = undefined;
    const n = f.readAll(buf[0..expect_len]) catch { w("readAll FAIL\n"); return; };
    if (n != expect_len) { w("F: SHORT READ (writev wrote wrong length)\n"); return; }
    // verify content
    var all: [64]u8 = undefined;
    var o: usize = 0;
    for ([_][]const u8{ &hdr, &tags, &data, &s1, &s2 }) |part| { @memcpy(all[o..][0..part.len], part); o += part.len; }
    if (!std.mem.eql(u8, buf[0..expect_len], all[0..expect_len])) { w("G: CONTENT MISMATCH (writev corrupted)\n"); return; }
    // verify stat size after
    const st = f.stat() catch { w("stat FAIL\n"); return; };
    var ob: [48]u8 = undefined;
    const so = std.fmt.bufPrint(&ob, "H: ok cache write, size={d} expect={d}\n", .{ st.size, expect_len }) catch return;
    w(so);
}
