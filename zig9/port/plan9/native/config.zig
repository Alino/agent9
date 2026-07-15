// zig9: build_options module for the native (on-9front) zig, byte-for-byte the
// config bootstrap.c writes (vendor/zig/bootstrap.c:121-146) for a no-LLVM
// bootstrap compiler.
pub const have_llvm = false;
pub const llvm_has_m68k = false;
pub const llvm_has_csky = false;
pub const llvm_has_arc = false;
pub const llvm_has_xtensa = false;
pub const version: [:0]const u8 = "0.14.1";
pub const semver = @import("std").SemanticVersion.parse(version) catch unreachable;
pub const enable_debug_extensions = false;
pub const enable_logging = false;
pub const enable_link_snapshots = false;
pub const enable_tracy = false;
pub const value_tracing = false;
pub const skip_non_native = false;
pub const debug_gpa = false;
pub const dev = .core;
pub const value_interpret_mode = .direct;
// zig9: the single-threaded plan9 build uses DebugAllocator (SmpAllocator
// asserts !single_threaded); it reads mem_leak_frames. 0 = no leak backtraces,
// matching a -OReleaseSmall/strip build (build.zig:184-185).
pub const mem_leak_frames: u32 = 0;
