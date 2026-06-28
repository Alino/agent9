const std = @import("std");
const p = std.os.plan9;
fn out(s: []const u8) void { _ = p.write(1, s.ptr, s.len); }
const E = error{ TooBig, Negative };
fn checked(x: i32) E!i32 { if (x < 0) return E.Negative; if (x > 100) return E.TooBig; return x * 2; }
pub fn main() void {
    const a = checked(10) catch return out("FAIL 09_errors a\n");
    if (a != 20) return out("FAIL 09_errors val\n");
    if (checked(-1)) |_| return out("FAIL 09_errors neg\n") else |e| if (e != E.Negative) return out("FAIL 09_errors etype\n");
    out("ok 09_errors\n");
}
