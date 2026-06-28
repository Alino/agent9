const std = @import("std");
const p = std.os.plan9;
fn out(s: []const u8) void { _ = p.write(1, s.ptr, s.len); }
noinline fn v(x: f64) f64 { return x; }
pub fn main() void {
    const r = std.math.sqrt(v(2.0));
    if (!(r > 1.4141 and r < 1.4143)) return out("FAIL 02_floats\n");
    if (std.math.floor(v(3.7)) != 3.0) return out("FAIL 02_floats floor\n");
    out("ok 02_floats\n");
}
