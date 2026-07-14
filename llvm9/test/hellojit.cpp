// hellojit.cpp — prove LLVM's codegen+JIT run ON 9front: build IR for
// `i32 @f(){ ret 42 }`, JIT-compile it with MCJIT (X86 backend), call it.
// The one 9front-specific piece: a memory manager that allocates code pages
// READ|WRITE|EXEC up front (cc9's mmap routes PROT_EXEC to segattach(SG_EXEC),
// which the wxallow kernel makes executable) — LLVM's default does mmap(RW)
// then mprotect(RX), and 9front can't upgrade malloc'd pages to executable.
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/ExecutionEngine/ExecutionEngine.h"
#include "llvm/ExecutionEngine/MCJIT.h"
#include "llvm/ExecutionEngine/RTDyldMemoryManager.h"
#include "llvm/Support/TargetSelect.h"
#include <sys/mman.h>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <string>

using namespace llvm;

// RWX-upfront allocator: hand JIT sections executable pages directly.
class RWXMemMgr : public RTDyldMemoryManager {
  uint8_t *alloc(uintptr_t Size, unsigned Align) {
    if (!Align) Align = 16;
    size_t sz = (Size + 4095) & ~(size_t)4095;   // page multiple
    void *p = mmap(nullptr, sz ? sz : 4096,
                   PROT_READ | PROT_WRITE | PROT_EXEC,
                   MAP_PRIVATE | MAP_ANON, -1, 0);
    return p == MAP_FAILED ? nullptr : (uint8_t *)p;
  }
public:
  uint8_t *allocateCodeSection(uintptr_t Size, unsigned Align, unsigned,
                               StringRef) override { return alloc(Size, Align); }
  uint8_t *allocateDataSection(uintptr_t Size, unsigned Align, unsigned,
                               StringRef, bool) override { return alloc(Size, Align); }
  bool finalizeMemory(std::string *) override { return false; }  // already exec
};

int main() {
  InitializeNativeTarget();
  InitializeNativeTargetAsmPrinter();

  auto Ctx = std::make_unique<LLVMContext>();
  auto M = std::make_unique<Module>("m", *Ctx);
  Module *Mod = M.get();
  IRBuilder<> B(*Ctx);
  FunctionType *FT = FunctionType::get(B.getInt32Ty(), false);
  Function *F = Function::Create(FT, Function::ExternalLinkage, "f", Mod);
  B.SetInsertPoint(BasicBlock::Create(*Ctx, "entry", F));
  B.CreateRet(B.getInt32(42));

  std::string Err;
  ExecutionEngine *EE =
      EngineBuilder(std::move(M))
          .setErrorStr(&Err)
          .setEngineKind(EngineKind::JIT)
          .setMCJITMemoryManager(std::make_unique<RWXMemMgr>())
          .create();
  if (!EE) { printf("EE create failed: %s\n", Err.c_str()); return 1; }
  EE->finalizeObject();

  auto fp = (int (*)())EE->getFunctionAddress("f");
  if (!fp) { printf("no address for f\n"); return 1; }
  printf("LLVM JIT on 9front: f() = %d (expect 42)\n", fp());
  return 0;
}
