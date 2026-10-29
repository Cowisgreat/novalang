// novac: the NovaLang compiler driver.
//
//   novac program.nov                 -> program.out (native executable)
//   novac program.nov -o app -O2      -> optimized executable `app`
//   novac program.nov --run           -> JIT-compile and run in memory
//   novac program.nov --emit-llvm     -> print LLVM IR to stdout
//   novac program.nov --emit-ast      -> print the parsed AST
//   novac program.nov --check         -> type-check only

#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>
#include <llvm/Support/raw_ostream.h>
#include <llvm/Target/TargetMachine.h>

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <memory>
#include <sstream>
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
  std::cerr << "usage: novac <file.nov> [options]\n"
               "  -o <file>       output executable (default: <file>.out)\n"
               "  -O0 .. -O3      optimization level (default -O0)\n"
               "  --run           JIT-compile and run without writing an executable\n"
               "  --emit-llvm     print LLVM IR\n"
               "  --emit-ast      print the syntax tree\n"
               "  --check         parse and type-check only\n"
               "  --stats         report runtime checks emitted and elided\n"
               "  --no-bce        keep bounds checks even where they are provably redundant\n"
               "  --unchecked     omit all runtime safety checks\n";
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
  bool emitLLVM = false, emitAST = false, checkOnly = false, run = false, stats = false;
  int optLevel = 0;
  nova::CodeGenOptions opts;

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
    } else if (arg == "--run") {
      run = true;
    } else if (arg == "--stats") {
      stats = true;
    } else if (arg == "--no-bce") {
      opts.boundsCheckElim = false;
    } else if (arg == "--unchecked") {
      opts.runtimeChecks = false;
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

  std::ifstream file(input);
  if (!file) {
    std::cerr << "novac: cannot open '" << input << "'\n";
    return 2;
  }
  std::stringstream buffer;
  buffer << file.rdbuf();
  std::string source = buffer.str();
  nova::diag().setFile(input);
  nova::diag().setSource(source);

  FILE *in = fmemopen(source.data(), source.size(), "r");
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

  std::string err;
  auto tm = nova::createHostTargetMachine(optLevel, err);
  if (!tm) {
    std::cerr << "novac: cannot target this machine: " << err << "\n";
    return 3;
  }
  auto ctx = std::make_unique<llvm::LLVMContext>();
  nova::CodeGenStats codegenStats;
  auto mod = nova::generateIR(*prog, *ctx, input, *tm, opts, codegenStats);
  if (!mod) return 3;
  nova::optimize(*mod, optLevel, tm.get());

  if (stats) {
    std::cerr << "bounds checks:   " << codegenStats.boundsChecks << " emitted, " << codegenStats.boundsChecksElided
              << " elided\n"
              << "division checks: " << codegenStats.divisionChecks << " emitted, "
              << codegenStats.divisionChecksElided << " elided\n";
  }

  if (emitLLVM) {
    mod->print(llvm::outs(), nullptr);
    return 0;
  }

  if (run) {
    int rc = nova::runJIT(std::move(mod), std::move(ctx), err);
    if (rc < 0 && !err.empty()) {
      std::cerr << "novac: JIT failed: " << err << "\n";
      return 3;
    }
    return rc;
  }

  if (output.empty()) output = defaultOutput(input);
  std::string obj = output + ".o";
  if (!nova::emitObject(*mod, obj, *tm, err)) {
    std::cerr << "novac: failed to emit object file: " << err << "\n";
    return 3;
  }

  const char *cc = std::getenv("CC");
  std::string linker = cc ? cc : "cc";
  std::string cmd = linker + " '" + obj + "' -o '" + output + "' -lm";
  int rc = std::system(cmd.c_str());
  unlink(obj.c_str());
  if (rc != 0) {
    std::cerr << "novac: linking failed\n";
    return 3;
  }
  return 0;
}
