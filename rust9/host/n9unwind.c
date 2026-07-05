/* Shims for the three libunwind entry points cc9's libunwind build did not
 * export but Rust's DWARF EH personality (std::sys::personality::gcc) needs.
 * All three are expressible from _Unwind_GetIP (which cc9 does export). */
typedef void _Unwind_Context;
extern unsigned long _Unwind_GetIP(_Unwind_Context *);

/* IP + "is this the currently-executing insn?" flag. For normal call-frame
 * unwinding the IP is a return address, so ip_before_insn = 0 (the personality
 * then looks up ip-1). */
unsigned long _Unwind_GetIPInfo(_Unwind_Context *ctx, int *ip_before_insn)
{
	*ip_before_insn = 0;
	return _Unwind_GetIP(ctx);
}

/* DW_EH_PE_datarel / textrel bases. x86_64 LSDAs use pcrel/absptr encodings,
 * never these, so 0 is never actually consulted. */
unsigned long _Unwind_GetDataRelBase(_Unwind_Context *ctx) { (void)ctx; return 0; }
unsigned long _Unwind_GetTextRelBase(_Unwind_Context *ctx) { (void)ctx; return 0; }
