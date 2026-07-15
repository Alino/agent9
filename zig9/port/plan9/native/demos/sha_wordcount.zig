// Integer/bitwise + allocator + hashmap heavy demo: generate ~1 MB of
// deterministic pseudo-words, SHA-256 the stream, and build a word-frequency
// table with std.AutoHashMap. Self-verifying (prints digest + top counts).
// Exercises std.crypto.hash.sha2, std.hash.Wyhash, AutoHashMap, sorting.
const std = @import("std");

// PCG32 — deterministic, portable.
var pcg_state: u64 = 0x853c49e6748fea9b;
const pcg_inc: u64 = 0xda3e39cb94b95bdb;
fn pcg32() u32 {
    const old = pcg_state;
    pcg_state = old *% 6364136223846793005 +% pcg_inc;
    const xorshifted: u32 = @truncate(((old >> 18) ^ old) >> 27);
    const rot: u5 = @truncate(old >> 59);
    return std.math.rotr(u32, xorshifted, rot);
}

pub fn main() !void {
    var gpa = std.heap.GeneralPurposeAllocator(.{}){};
    defer _ = gpa.deinit();
    const a = gpa.allocator();

    // A small vocabulary; pick words by PCG so the stream is deterministic.
    const vocab = [_][]const u8{
        "plan9", "zig", "compiler", "native", "sphere", "ray", "glenda",
        "kernel", "acme", "rio", "venti", "fossil", "libc", "syscall",
        "atom", "cache", "build", "link", "hash", "table",
    };

    const n_words: usize = 120_000;
    var sha = std.crypto.hash.sha2.Sha256.init(.{});
    var counts = std.AutoHashMap(u32, u32).init(a);
    defer counts.deinit();

    var total_bytes: usize = 0;
    var i: usize = 0;
    while (i < n_words) : (i += 1) {
        const idx: u32 = @intCast(pcg32() % vocab.len);
        const word = vocab[idx];
        sha.update(word);
        sha.update(" ");
        total_bytes += word.len + 1;
        const gop = try counts.getOrPut(idx);
        if (gop.found_existing) gop.value_ptr.* += 1 else gop.value_ptr.* = 1;
    }

    var digest: [32]u8 = undefined;
    sha.final(&digest);

    // Sort vocab indices by count, descending, for a stable top-3 report.
    const Pair = struct { idx: u32, count: u32 };
    var pairs = std.ArrayList(Pair).init(a);
    defer pairs.deinit();
    var it = counts.iterator();
    while (it.next()) |e| try pairs.append(.{ .idx = e.key_ptr.*, .count = e.value_ptr.* });
    std.mem.sort(Pair, pairs.items, {}, struct {
        fn lt(_: void, x: Pair, y: Pair) bool {
            if (x.count != y.count) return x.count > y.count;
            return x.idx < y.idx;
        }
    }.lt);

    const so = std.io.getStdOut().writer();
    try so.print("sha_wordcount: {d} words, {d} bytes, {d} distinct\n", .{ n_words, total_bytes, pairs.items.len });
    try so.writeAll("sha256=");
    for (digest) |b| try so.print("{x:0>2}", .{b});
    try so.writeAll("\n");
    const top = @min(pairs.items.len, 3);
    for (pairs.items[0..top]) |p| {
        try so.print("  {s}: {d}\n", .{ vocab[p.idx], p.count });
    }
}
