// LLVM IR generation and native code emission.
#pragma once

#include <memory>
#include <string>

#include "ast.h"

namespace llvm {
class LLVMContext;
class Module;
}  // namespace llvm

namespace nova {

// Lowers a type-checked program to an LLVM module.
std::unique_ptr<llvm::Module> generateIR(const Program &prog, llvm::LLVMContext &ctx,
                                         const std::string &moduleName);

// Runs the standard LLVM optimization pipeline at the given level (0-3).
void optimize(llvm::Module &mod, int level);

// Writes a native object file for the host target. Returns false on failure.
bool emitObject(llvm::Module &mod, const std::string &path, int optLevel, std::string &err);

}  // namespace nova
