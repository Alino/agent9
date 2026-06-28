const std = @import("std");
const p = std.os.plan9;
fn out(s: []const u8) void { _ = p.write(1, s.ptr, s.len); }
noinline fn v(x: u32) u32 { return x; }
pub fn main() void {
    if (@popCount(v(0xFF)) != 8) return out("FAIL 08_bitops popcount\n");
    if (@clz(v(1)) != 31) return out("FAIL 08_bitops clz\n");
    if ((v(0b1010) ^ v(0b0110)) != 0b1100) return out("FAIL 08_bitops xor\n");
    out("ok 08_bitops\n");
}
