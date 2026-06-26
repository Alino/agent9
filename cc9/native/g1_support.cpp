// G1 probe: a trivial program that links LLVM's Support library and runs on
// 9front. Exercises StringRef, Triple parsing, and raw_ostream (file I/O to
// fd 1) — enough to drag in the libc/OS surface that LLVM Support needs.
#include "llvm/ADT/StringRef.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/TargetParser/Triple.h"
#include "llvm/TargetParser/Host.h"

int main(int argc, char **argv) {
  llvm::Triple T(argc > 1 ? argv[1] : "x86_64-unknown-none-elf");
  llvm::outs() << "g1_support: triple=" << T.str()
               << " arch=" << T.getArchName()
               << " os=" << (T.getOSName().empty() ? "none" : T.getOSName())
               << " ptrwidth=" << (T.isArch64Bit() ? 64 : 32) << "\n";
  llvm::StringRef s = "9front-os";
  auto parts = s.split('-');
  llvm::outs() << "stringref: " << s << " len=" << s.size()
               << " split=[" << parts.first << "|" << parts.second << "]"
               << " upper=" << s.upper() << "\n";
  return 0;
}
