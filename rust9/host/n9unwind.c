/* n9unwind.c — INTENTIONALLY EMPTY (defines no external symbols).
 *
 * This file once shimmed three libunwind entry points Rust's DWARF-EH personality
 * (std::sys::personality::gcc) needs — _Unwind_GetIPInfo, _Unwind_GetDataRelBase,
 * _Unwind_GetTextRelBase — back when cc9's libunwind build didn't export them.
 *
 * cc9's libcc9cxx.a now exports the REAL three (verified: `T _Unwind_GetIPInfo`
 * etc.). Keeping the old stubs here was actively harmful in the RELEASE link: the
 * prebuilt n9link hardcodes linking `$N9LINK_LIB/n9unwind.o` alongside the archive,
 * so the stub `_Unwind_GetIPInfo` (which always reported ip_before_insn = 0, losing
 * the signal/note-frame distinction) could win over libcc9cxx.a's correct one —
 * making released binaries unwind differently from dev builds (rust9-ld, which
 * already dropped the shim). Emptying the file — rather than deleting it — keeps
 * n9link's hardcoded n9unwind.o path satisfied (the object still exists) while
 * defining nothing, so the real symbols from libcc9cxx.a are always the ones used.
 *
 * Do not add symbols here. If cc9 ever stops exporting a libunwind entry point,
 * add it to cc9's runtime, not to this shadow object.
 */
