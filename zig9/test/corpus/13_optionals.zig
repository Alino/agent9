const std = @import("std");
const p = std.os.plan9;
fn out(s: []const u8) void { _ = p.write(1, s.ptr, s.len); }
fn find(haystack: []const u8, needle: u8) ?usize {
    for (haystack, 0..) |c, i| if (c == needle) return i;
    return null;
}
pub fn main() void {
    const idx = find("plan9", '9') orelse return out("FAIL 13_optionals\n");
    if (idx != 4) return out("FAIL 13_optionals idx\n");
    if (find("abc", 'z') != null) return out("FAIL 13_optionals null\n");
    out("ok 13_optionals\n");
}
