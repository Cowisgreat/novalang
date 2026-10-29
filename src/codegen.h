// LLVM IR generation, native code emission and JIT execution.
#pragma once

#include <memory>
#include <string>

#include "ast.h"

namespace llvm {
class LLVMContext;
class Module;
class TargetMachine;
}  // namespace llvm

namespace nova {

struct CodeGenOptions {
  bool runtimeChecks = true;      // bounds and division-by-zero checks (--unchecked disables)
  bool boundsCheckElim = true;    // skip checks sema proved redundant (--no-bce disables)
};

struct CodeGenStats {
  int boundsChecks = 0;           // array accesses that got a runtime check
  int boundsChecksElided = 0;     // array accesses proven safe at compile time
  int divisionChecks = 0;
  int divisionChecksElided = 0;   // divisor is a non-zero constant
};

// Lowers a type-checked program to an LLVM module.
std::unique_ptr<llvm::Module> generateIR(const Program &prog, llvm::LLVMContext &ctx, const std::string &moduleName,
                                         const CodeGenOptions &opts, CodeGenStats &stats);

// A TargetMachine for the host CPU and its features (like -march=native). The module's
// data layout and triple are set from it so the optimizer sees the real target.
std::unique_ptr<llvm::TargetMachine> createHostTargetMachine(llvm::Module &mod, int optLevel, std::string &err);

// Runs the standard LLVM optimization pipeline at the given level (0-3). The target
// machine supplies the cost model (TargetTransformInfo); without it the vectorizers
// assume a vector width of 1 and never fire.
void optimize(llvm::Module &mod, int level, llvm::TargetMachine *tm);

// Writes a native object file. Returns false on failure.
bool emitObject(llvm::Module &mod, const std::string &path, llvm::TargetMachine &tm, std::string &err);

// Compiles the module in memory with LLVM's ORC JIT and runs its `main`.
// Returns the program's exit code, or -1 with `err` set on failure.
int runJIT(std::unique_ptr<llvm::Module> mod, std::unique_ptr<llvm::LLVMContext> ctx, std::string &err);

}  // namespace nova
