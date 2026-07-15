// zig9 fs shakeout: exercises the exact std.fs / std.time plan9 arms the native
// compiler's Cache + Compilation use, built through the same CBE+cc9 pipeline as
// the compiler but tiny (~seconds to compile). Prints "ok fs_smoke" on success.
// Run before the full compiler build so a broken fs arm costs seconds, not an
// hour. void main + catch (the plan9 build rule).
const std = @import("std");

fn die(comptime msg: []const u8) noreturn {
    std.debug.print("FAIL: {s}\n", .{msg});
    std.process.exit(1);
}

var step: []const u8 = "start";
pub fn main() void {
    run() catch |e| {
        std.debug.print("FAIL at [{s}]: {s} errstr={s}\n", .{ step, @errorName(e), std.os.plan9.errstr() });
        std.process.exit(1);
    };
    std.debug.print("ok fs_smoke\n", .{});
}

fn run() !void {
    var buf: [4096]u8 = undefined;
    var fba = std.heap.FixedBufferAllocator.init(&buf);
    const a = fba.allocator();

    // getcwd
    var cwd_buf: [std.fs.max_path_bytes]u8 = undefined;
    const cwd = try std.posix.getcwd(&cwd_buf);
    std.debug.print("cwd={s}\n", .{cwd});

    var dir = std.fs.cwd();

    step = "makePath";
    try dir.makePath("zig9smoke/sub");
    std.debug.print("did makePath\n", .{});
    {
        var f = try dir.createFile("zig9smoke/sub/a.txt", .{ .truncate = true });
        defer f.close();
        try f.writeAll("hello zig9");
    }

    std.debug.print("did createFile\n", .{});
    step = "statFile";
    {
        const st = try dir.statFile("zig9smoke/sub/a.txt");
        if (st.size != 10) die("stat size wrong");
        if (st.kind != .file) die("stat kind wrong");
    }

    std.debug.print("did statFile\n", .{});
    step = "lock";
    {
        var f = try dir.openFile("zig9smoke/sub/a.txt", .{ .mode = .read_write });
        defer f.close();
        try f.lock(.exclusive);
        f.unlock();
    }

    std.debug.print("did lock\n", .{});
    step = "rename";
    {
        // direct rename (isolates my wstat rename from AtomicFile's randomness)
        {
            var f = try dir.createFile("zig9smoke/sub/tmp.txt", .{ .truncate = true });
            f.writeAll("atomic") catch {};
            f.close();
        }
        std.debug.print("  rename tmp.txt -> b.txt\n", .{});
        try dir.rename("zig9smoke/sub/tmp.txt", "zig9smoke/sub/b.txt");
    }
    {
        const st = try dir.statFile("zig9smoke/sub/b.txt");
        if (st.size != 6) die("rename size wrong");
    }

    std.debug.print("did atomicFile\n", .{});
    step = "readback";
    {
        const contents = try dir.readFileAlloc(a, "zig9smoke/sub/a.txt", 64);
        if (!std.mem.eql(u8, contents, "hello zig9")) die("read-back mismatch");
    }

    std.debug.print("did readback\n", .{});
    step = "iterate";
    {
        var d = try dir.openDir("zig9smoke/sub", .{ .iterate = true });
        defer d.close();
        var it = d.iterate();
        var count: usize = 0;
        while (try it.next()) |ent| {
            if (ent.kind == .file) count += 1;
        }
        if (count != 2) die("dir iterate count wrong");
    }

    std.debug.print("did iterate\n", .{});
    step = "delete";
    try dir.deleteFile("zig9smoke/sub/a.txt");
    try dir.deleteFile("zig9smoke/sub/b.txt");
    std.debug.print("  deleted files\n", .{});
    try dir.deleteDir("zig9smoke/sub");
    std.debug.print("  deleted sub\n", .{});
    try dir.deleteDir("zig9smoke");
    std.debug.print("  deleted zig9smoke\n", .{});

    std.debug.print("did delete\n", .{});
    step = "time";
    const t0 = std.time.nanoTimestamp();
    if (t0 == 0) die("nanoTimestamp zero");

    // env: cc9 surfaces /env; at least PATH-ish "home" may exist, don't require.
    if (std.posix.getenv("home")) |h| std.debug.print("home={s}\n", .{h});
}
