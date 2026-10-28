// novac: the NovaLang compiler driver.
//
//   novac program.nov                 -> program.out (native executable)
//   novac program.nov -o app          -> app
//   novac program.nov --emit-llvm     -> print LLVM IR to stdout
//   novac program.nov --emit-ast      -> print the parsed AST
//   novac program.nov --check         -> type-check only

#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>
#include <llvm/Support/raw_ostream.h>

#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <string>
#include <unistd.h>

#include "ast.h"
#include "codegen.h"
#include "diagnostics.h"
#include "sema.h"

extern int yyparse();
extern nova::Program *novaParsedProgram;
void novaLexerReset(FILE *in);

namespace {

void usage() {
  std::cerr << "usage: novac <file.nov> [-o output] [-O0|-O1|-O2|-O3] [--emit-llvm] [--emit-ast] [--check]\n";
}

std::string defaultOutput(const std::string &input) {
  std::string base = input;
  auto slash = base.find_last_of('/');
  if (slash != std::string::npos) base = base.substr(slash + 1);
  auto dot = base.rfind(".nov");
  if (dot != std::string::npos && dot + 4 == base.size()) base = base.substr(0, dot);
  return base + ".out";
}

}  // namespace

int main(int argc, char **argv) {
  std::string input, output;
  bool emitLLVM = false, emitAST = false, checkOnly = false;
  int optLevel = 0;

  for (int i = 1; i < argc; ++i) {
    std::string arg = argv[i];
    if (arg == "-o" && i + 1 < argc) {
      output = argv[++i];
    } else if (arg == "--emit-llvm") {
      emitLLVM = true;
    } else if (arg == "--emit-ast") {
      emitAST = true;
    } else if (arg == "--check") {
      checkOnly = true;
    } else if (arg.size() == 3 && arg[0] == '-' && arg[1] == 'O' && arg[2] >= '0' && arg[2] <= '3') {
      optLevel = arg[2] - '0';
    } else if (arg == "-h" || arg == "--help") {
      usage();
      return 0;
    } else if (!arg.empty() && arg[0] == '-') {
      std::cerr << "novac: unknown option '" << arg << "'\n";
      usage();
      return 2;
    } else if (input.empty()) {
      input = arg;
    } else {
      std::cerr << "novac: multiple input files given\n";
      return 2;
    }
  }
  if (input.empty()) {
    usage();
    return 2;
  }

  FILE *in = std::fopen(input.c_str(), "r");
  if (!in) {
    std::cerr << "novac: cannot open '" << input << "'\n";
    return 2;
  }
  nova::diag().setFile(input);
  novaLexerReset(in);
  int parseResult = yyparse();
  std::fclose(in);
  std::unique_ptr<nova::Program> prog(novaParsedProgram);
  if (parseResult != 0 || nova::diag().hasErrors() || !prog) {
    std::cerr << nova::diag().errorCount() << " error(s) generated.\n";
    return 1;
  }

  if (emitAST) {
    nova::dumpAst(*prog);
    return 0;
  }

  nova::analyze(*prog);
  if (nova::diag().hasErrors()) {
    std::cerr << nova::diag().errorCount() << " error(s) generated.\n";
    return 1;
  }
  if (checkOnly) return 0;

  llvm::LLVMContext ctx;
  auto mod = nova::generateIR(*prog, ctx, input);
  if (!mod) return 3;
  nova::optimize(*mod, optLevel);

  if (emitLLVM) {
    mod->print(llvm::outs(), nullptr);
    return 0;
  }

  if (output.empty()) output = defaultOutput(input);
  std::string obj = output + ".o";
  std::string err;
  if (!nova::emitObject(*mod, obj, optLevel, err)) {
    std::cerr << "novac: failed to emit object file: " << err << "\n";
    return 3;
  }

  const char *cc = std::getenv("CC");
  std::string linker = cc ? cc : "cc";
  std::string cmd = linker + " '" + obj + "' -o '" + output + "'";
  int rc = std::system(cmd.c_str());
  unlink(obj.c_str());
  if (rc != 0) {
    std::cerr << "novac: linking failed\n";
    return 3;
  }
  return 0;
}
