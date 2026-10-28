#include "codegen.h"

#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/LegacyPassManager.h>
#include <llvm/IR/MDBuilder.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/Verifier.h>
#include <llvm/MC/TargetRegistry.h>
#include <llvm/Passes/PassBuilder.h>
#include <llvm/Support/FileSystem.h>
#include <llvm/Support/TargetSelect.h>
#include <llvm/Support/raw_ostream.h>
#include <llvm/Target/TargetMachine.h>
#include <llvm/Target/TargetOptions.h>
#include <llvm/TargetParser/Host.h>

#include <map>
#include <vector>

namespace nova {

namespace {

// User functions are emitted under a prefix so they never collide with libc
// symbols (a NovaLang function named `printf` or `main` is perfectly legal).
std::string mangle(const std::string &name) { return "nova." + name; }

class CodeGen {
 public:
  CodeGen(llvm::LLVMContext &ctx, const std::string &name)
      : ctx_(ctx), mod_(std::make_unique<llvm::Module>(name, ctx)), b_(ctx), file_(name) {}

  std::unique_ptr<llvm::Module> run(const Program &prog) {
    declareRuntime();
    for (auto &f : prog.functions) declareFunction(*f);
    for (auto &f : prog.functions) defineFunction(*f);
    emitEntryPoint(prog);
    return std::move(mod_);
  }

 private:
  struct Loop {
    llvm::BasicBlock *continueTarget;
    llvm::BasicBlock *breakTarget;
  };

  llvm::LLVMContext &ctx_;
  std::unique_ptr<llvm::Module> mod_;
  llvm::IRBuilder<> b_;
  std::string file_;
  llvm::Function *printf_ = nullptr;
  llvm::Function *dprintf_ = nullptr;
  llvm::Function *malloc_ = nullptr;
  llvm::Function *exit_ = nullptr;
  llvm::Function *current_ = nullptr;
  std::vector<std::map<std::string, llvm::AllocaInst *>> scopes_;
  std::vector<Loop> loops_;

  // Arrays are fat pointers: { element*, length }. Units exist only at compile time.
  llvm::StructType *arrayTy() { return llvm::StructType::get(ctx_, {b_.getPtrTy(), b_.getInt64Ty()}); }

  llvm::Type *lowerScalar(Kind k) {
    switch (k) {
      case Kind::Int: return b_.getInt64Ty();
      case Kind::Float: return b_.getDoubleTy();
      case Kind::Bool: return b_.getInt1Ty();
      case Kind::String: return b_.getPtrTy();
      case Kind::Void:
      case Kind::Unknown: break;
    }
    return b_.getVoidTy();
  }

  llvm::Type *lower(const Type &t) { return t.array ? arrayTy() : lowerScalar(t.kind); }

  void declareRuntime() {
    auto *ptr = b_.getPtrTy();
    printf_ = llvm::Function::Create(llvm::FunctionType::get(b_.getInt32Ty(), {ptr}, true),
                                     llvm::Function::ExternalLinkage, "printf", *mod_);
    dprintf_ = llvm::Function::Create(llvm::FunctionType::get(b_.getInt32Ty(), {b_.getInt32Ty(), ptr}, true),
                                      llvm::Function::ExternalLinkage, "dprintf", *mod_);
    malloc_ = llvm::Function::Create(llvm::FunctionType::get(ptr, {b_.getInt64Ty()}, false),
                                     llvm::Function::ExternalLinkage, "malloc", *mod_);
    exit_ = llvm::Function::Create(llvm::FunctionType::get(b_.getVoidTy(), {b_.getInt32Ty()}, false),
                                   llvm::Function::ExternalLinkage, "exit", *mod_);
    exit_->setDoesNotReturn();
  }

  void declareFunction(const Function &f) {
    std::vector<llvm::Type *> params;
    for (auto &p : f.params) params.push_back(lower(p.type));
    auto *ty = llvm::FunctionType::get(lower(f.returnType), params, false);
    auto *fn = llvm::Function::Create(ty, llvm::Function::InternalLinkage, mangle(f.name), *mod_);
    unsigned i = 0;
    for (auto &arg : fn->args()) arg.setName(f.params[i++].name);
  }

  llvm::AllocaInst *createEntryAlloca(llvm::Type *ty, const std::string &name) {
    llvm::IRBuilder<> tmp(&current_->getEntryBlock(), current_->getEntryBlock().begin());
    return tmp.CreateAlloca(ty, nullptr, name);
  }

  llvm::AllocaInst *lookup(const std::string &name) {
    for (auto it = scopes_.rbegin(); it != scopes_.rend(); ++it) {
      auto found = it->find(name);
      if (found != it->end()) return found->second;
    }
    return nullptr;
  }

  bool terminated() { return b_.GetInsertBlock()->getTerminator() != nullptr; }

  llvm::BasicBlock *newBlock(const char *name) { return llvm::BasicBlock::Create(ctx_, name, current_); }

  // Emits `if (failed) { report; exit(101); }` and continues in the success path.
  void guard(llvm::Value *failed, Loc loc, const std::string &msg, std::vector<llvm::Value *> extra = {}) {
    auto *failBB = newBlock("panic");
    auto *okBB = newBlock("ok");
    b_.CreateCondBr(failed, failBB, okBB, llvm::MDBuilder(ctx_).createBranchWeights(1, 1 << 20));
    b_.SetInsertPoint(failBB);
    std::string text = "runtime error at " + file_ + ":" + std::to_string(loc.line) + ":" +
                       std::to_string(loc.col) + ": " + msg + "\n";
    std::vector<llvm::Value *> args = {b_.getInt32(2), b_.CreateGlobalStringPtr(text, "panic.msg")};
    args.insert(args.end(), extra.begin(), extra.end());
    b_.CreateCall(dprintf_, args);
    b_.CreateCall(exit_, {b_.getInt32(101)});
    b_.CreateUnreachable();
    b_.SetInsertPoint(okBB);
  }

  void defineFunction(const Function &f) {
    current_ = mod_->getFunction(mangle(f.name));
    auto *entry = llvm::BasicBlock::Create(ctx_, "entry", current_);
    b_.SetInsertPoint(entry);
    scopes_.clear();
    scopes_.emplace_back();
    unsigned i = 0;
    for (auto &arg : current_->args()) {
      auto *slot = createEntryAlloca(arg.getType(), f.params[i].name);
      b_.CreateStore(&arg, slot);
      scopes_.back()[f.params[i].name] = slot;
      ++i;
    }
    genBlock(*f.body);
    if (!terminated()) {
      if (f.returnType.kind == Kind::Void)
        b_.CreateRetVoid();
      else
        b_.CreateUnreachable();  // sema guarantees every path returns
    }
  }

  // C entry point: calls nova.main and returns its result as the exit code.
  void emitEntryPoint(const Program &prog) {
    const Function *userMain = nullptr;
    for (auto &f : prog.functions)
      if (f->name == "main") userMain = f.get();
    if (!userMain) return;

    auto *ty = llvm::FunctionType::get(b_.getInt32Ty(), false);
    auto *fn = llvm::Function::Create(ty, llvm::Function::ExternalLinkage, "main", *mod_);
    b_.SetInsertPoint(llvm::BasicBlock::Create(ctx_, "entry", fn));
    llvm::Value *rv = b_.CreateCall(mod_->getFunction(mangle("main")));
    if (userMain->returnType.kind == Kind::Int)
      b_.CreateRet(b_.CreateTrunc(rv, b_.getInt32Ty()));
    else
      b_.CreateRet(b_.getInt32(0));
  }

  // ------------------------------------------------------------- statements

  void genBlock(const Block &blk) {
    scopes_.emplace_back();
    for (auto &s : blk.stmts) {
      if (terminated()) break;  // unreachable code after return/break/continue
      genStmt(*s);
    }
    scopes_.pop_back();
  }

  // Runs `body(i)` for i in [start, end); `continue` jumps to the increment.
  template <typename Body>
  void genCountedLoop(llvm::Value *start, llvm::Value *end, const std::string &name, Body body) {
    auto *counter = createEntryAlloca(b_.getInt64Ty(), name + ".counter");
    b_.CreateStore(start, counter);
    auto *condBB = newBlock("for.cond");
    auto *bodyBB = newBlock("for.body");
    auto *stepBB = newBlock("for.step");
    auto *endBB = newBlock("for.end");
    b_.CreateBr(condBB);

    b_.SetInsertPoint(condBB);
    llvm::Value *i = b_.CreateLoad(b_.getInt64Ty(), counter, name);
    b_.CreateCondBr(b_.CreateICmpSLT(i, end), bodyBB, endBB);

    b_.SetInsertPoint(bodyBB);
    loops_.push_back({stepBB, endBB});
    body(b_.CreateLoad(b_.getInt64Ty(), counter));
    loops_.pop_back();
    if (!terminated()) b_.CreateBr(stepBB);

    b_.SetInsertPoint(stepBB);
    b_.CreateStore(b_.CreateAdd(b_.CreateLoad(b_.getInt64Ty(), counter), b_.getInt64(1)), counter);
    b_.CreateBr(condBB);

    endBB->moveAfter(&current_->back());
    b_.SetInsertPoint(endBB);
  }

  // Address of a[i] after a bounds check.
  llvm::Value *elementPtr(llvm::Value *arr, llvm::Value *idx, const Type &elem, Loc loc) {
    llvm::Value *len = b_.CreateExtractValue(arr, 1, "len");
    guard(b_.CreateICmpUGE(idx, len), loc, "index %lld out of bounds for array of length %lld", {idx, len});
    return b_.CreateGEP(lowerScalar(elem.kind), b_.CreateExtractValue(arr, 0), idx, "elem.ptr");
  }

  void genStmt(const Stmt &s) {
    if (auto *blk = dynamic_cast<const Block *>(&s)) return genBlock(*blk);

    if (auto *let = dynamic_cast<const LetStmt *>(&s)) {
      llvm::Value *v = genExpr(*let->init);
      auto *slot = createEntryAlloca(lower(let->declared), let->name);
      b_.CreateStore(v, slot);
      scopes_.back()[let->name] = slot;
      return;
    }

    if (auto *as = dynamic_cast<const AssignStmt *>(&s)) return genAssign(*as);

    if (auto *es = dynamic_cast<const ExprStmt *>(&s)) {
      genExpr(*es->expr);
      return;
    }

    if (auto *ret = dynamic_cast<const ReturnStmt *>(&s)) {
      if (ret->value)
        b_.CreateRet(genExpr(*ret->value));
      else
        b_.CreateRetVoid();
      return;
    }

    if (auto *is = dynamic_cast<const IfStmt *>(&s)) {
      llvm::Value *cond = genExpr(*is->cond);
      auto *thenBB = newBlock("if.then");
      auto *elseBB = is->elseBranch ? newBlock("if.else") : nullptr;
      auto *mergeBB = newBlock("if.end");
      b_.CreateCondBr(cond, thenBB, elseBB ? elseBB : mergeBB);

      b_.SetInsertPoint(thenBB);
      genStmt(*is->thenBranch);
      if (!terminated()) b_.CreateBr(mergeBB);

      if (elseBB) {
        b_.SetInsertPoint(elseBB);
        genStmt(*is->elseBranch);
        if (!terminated()) b_.CreateBr(mergeBB);
      }

      mergeBB->moveAfter(&current_->back());
      b_.SetInsertPoint(mergeBB);
      return;
    }

    if (auto *ws = dynamic_cast<const WhileStmt *>(&s)) {
      auto *condBB = newBlock("while.cond");
      auto *bodyBB = newBlock("while.body");
      auto *endBB = newBlock("while.end");
      b_.CreateBr(condBB);

      b_.SetInsertPoint(condBB);
      b_.CreateCondBr(genExpr(*ws->cond), bodyBB, endBB);

      b_.SetInsertPoint(bodyBB);
      loops_.push_back({condBB, endBB});
      genStmt(*ws->body);
      loops_.pop_back();
      if (!terminated()) b_.CreateBr(condBB);

      endBB->moveAfter(&current_->back());
      b_.SetInsertPoint(endBB);
      return;
    }

    if (auto *fs = dynamic_cast<const ForStmt *>(&s)) {
      // The range end is evaluated once, before the first iteration.
      llvm::Value *start = genExpr(*fs->start);
      llvm::Value *end = genExpr(*fs->end);
      genCountedLoop(start, end, fs->var, [&](llvm::Value *i) {
        // The loop variable is an immutable copy so the body cannot disturb iteration.
        auto *var = createEntryAlloca(b_.getInt64Ty(), fs->var);
        b_.CreateStore(i, var);
        scopes_.emplace_back();
        scopes_.back()[fs->var] = var;
        genStmt(*fs->body);
        scopes_.pop_back();
      });
      return;
    }

    if (auto *fe = dynamic_cast<const ForEachStmt *>(&s)) {
      llvm::Value *arr = genExpr(*fe->iterable);
      Type elem = fe->iterable->type.element();
      llvm::Value *len = b_.CreateExtractValue(arr, 1, "len");
      llvm::Value *data = b_.CreateExtractValue(arr, 0, "data");
      genCountedLoop(b_.getInt64(0), len, fe->var, [&](llvm::Value *i) {
        auto *var = createEntryAlloca(lowerScalar(elem.kind), fe->var);
        llvm::Type *ety = lowerScalar(elem.kind);
        b_.CreateStore(b_.CreateLoad(ety, b_.CreateGEP(ety, data, i)), var);
        scopes_.emplace_back();
        scopes_.back()[fe->var] = var;
        genStmt(*fe->body);
        scopes_.pop_back();
      });
      return;
    }

    if (dynamic_cast<const BreakStmt *>(&s)) {
      b_.CreateBr(loops_.back().breakTarget);
      return;
    }

    if (dynamic_cast<const ContinueStmt *>(&s)) {
      b_.CreateBr(loops_.back().continueTarget);
      return;
    }
  }

  void genAssign(const AssignStmt &as) {
    llvm::Value *ptr;
    Type type;
    if (auto *v = dynamic_cast<const VarRef *>(as.target.get())) {
      ptr = lookup(v->name);
      type = v->type;
    } else {
      auto *ix = static_cast<const Index *>(as.target.get());
      llvm::Value *arr = genExpr(*ix->base);
      llvm::Value *idx = genExpr(*ix->index);
      type = ix->type;
      ptr = elementPtr(arr, idx, type, ix->loc);
    }
    llvm::Value *value = genExpr(*as.value);
    if (as.compound) {
      llvm::Value *old = b_.CreateLoad(lower(type), ptr);
      value = genArith(as.op, old, value, type.kind, as.value->loc);
    }
    b_.CreateStore(value, ptr);
  }

  // ------------------------------------------------------------ expressions

  llvm::Value *genExpr(const Expr &e) {
    if (auto *n = dynamic_cast<const IntLit *>(&e)) return b_.getInt64(n->value);
    if (auto *n = dynamic_cast<const FloatLit *>(&e)) return llvm::ConstantFP::get(b_.getDoubleTy(), n->value);
    if (auto *n = dynamic_cast<const BoolLit *>(&e)) return b_.getInt1(n->value);
    if (auto *n = dynamic_cast<const StringLit *>(&e)) return b_.CreateGlobalStringPtr(n->value, "str");

    if (auto *n = dynamic_cast<const VarRef *>(&e)) {
      auto *slot = lookup(n->name);
      return b_.CreateLoad(slot->getAllocatedType(), slot, n->name);
    }

    if (auto *n = dynamic_cast<const Unary *>(&e)) {
      llvm::Value *v = genExpr(*n->operand);
      if (n->op == UnOp::Not) return b_.CreateNot(v);
      return n->type.kind == Kind::Float ? b_.CreateFNeg(v) : b_.CreateNeg(v);
    }

    if (auto *n = dynamic_cast<const Binary *>(&e)) return genBinary(*n);
    if (auto *n = dynamic_cast<const Call *>(&e)) return genCall(*n);
    if (auto *n = dynamic_cast<const Cast *>(&e)) return genCast(*n);
    if (auto *n = dynamic_cast<const ArrayLit *>(&e)) return genArrayLit(*n);
    if (auto *n = dynamic_cast<const ArrayRepeat *>(&e)) return genArrayRepeat(*n);

    if (auto *n = dynamic_cast<const Index *>(&e)) {
      llvm::Value *arr = genExpr(*n->base);
      llvm::Value *idx = genExpr(*n->index);
      return b_.CreateLoad(lowerScalar(n->type.kind), elementPtr(arr, idx, n->type, n->loc));
    }
    return nullptr;
  }

  llvm::Value *allocArray(const Type &elem, llvm::Value *len) {
    llvm::Type *ety = lowerScalar(elem.kind);
    llvm::Value *bytes = b_.CreateMul(llvm::ConstantExpr::getSizeOf(ety), len);
    llvm::Value *data = b_.CreateCall(malloc_, {bytes}, "data");
    llvm::Value *arr = llvm::UndefValue::get(arrayTy());
    arr = b_.CreateInsertValue(arr, data, 0);
    return b_.CreateInsertValue(arr, len, 1);
  }

  llvm::Value *genArrayLit(const ArrayLit &n) {
    Type elem = n.type.element();
    llvm::Type *ety = lowerScalar(elem.kind);
    llvm::Value *arr = allocArray(elem, b_.getInt64(n.elements.size()));
    llvm::Value *data = b_.CreateExtractValue(arr, 0);
    for (size_t i = 0; i < n.elements.size(); ++i)
      b_.CreateStore(genExpr(*n.elements[i]), b_.CreateGEP(ety, data, b_.getInt64(i)));
    return arr;
  }

  llvm::Value *genArrayRepeat(const ArrayRepeat &n) {
    Type elem = n.type.element();
    llvm::Type *ety = lowerScalar(elem.kind);
    llvm::Value *value = genExpr(*n.value);
    llvm::Value *len = genExpr(*n.count);
    guard(b_.CreateICmpSLT(len, b_.getInt64(0)), n.count->loc, "negative array length %lld", {len});
    llvm::Value *arr = allocArray(elem, len);
    llvm::Value *data = b_.CreateExtractValue(arr, 0);
    genCountedLoop(b_.getInt64(0), len, "fill",
                   [&](llvm::Value *i) { b_.CreateStore(value, b_.CreateGEP(ety, data, i)); });
    return arr;
  }

  llvm::Value *genShortCircuit(const Binary &n) {
    bool isAnd = n.op == BinOp::And;
    llvm::Value *lhs = genExpr(*n.lhs);
    auto *lhsBB = b_.GetInsertBlock();
    auto *rhsBB = newBlock(isAnd ? "and.rhs" : "or.rhs");
    auto *endBB = newBlock(isAnd ? "and.end" : "or.end");
    if (isAnd)
      b_.CreateCondBr(lhs, rhsBB, endBB);
    else
      b_.CreateCondBr(lhs, endBB, rhsBB);

    b_.SetInsertPoint(rhsBB);
    llvm::Value *rhs = genExpr(*n.rhs);
    rhsBB = b_.GetInsertBlock();  // rhs may itself have introduced blocks
    b_.CreateBr(endBB);

    endBB->moveAfter(&current_->back());
    b_.SetInsertPoint(endBB);
    auto *phi = b_.CreatePHI(b_.getInt1Ty(), 2);
    phi->addIncoming(b_.getInt1(!isAnd), lhsBB);
    phi->addIncoming(rhs, rhsBB);
    return phi;
  }

  llvm::Value *genBinary(const Binary &n) {
    if (n.op == BinOp::And || n.op == BinOp::Or) return genShortCircuit(n);
    llvm::Value *l = genExpr(*n.lhs);
    llvm::Value *r = genExpr(*n.rhs);
    return genArith(n.op, l, r, n.lhs->type.kind, n.loc);
  }

  llvm::Value *genArith(BinOp op, llvm::Value *l, llvm::Value *r, Kind operand, Loc loc) {
    if (operand == Kind::Float) {
      switch (op) {
        case BinOp::Add: return b_.CreateFAdd(l, r);
        case BinOp::Sub: return b_.CreateFSub(l, r);
        case BinOp::Mul: return b_.CreateFMul(l, r);
        case BinOp::Div: return b_.CreateFDiv(l, r);
        case BinOp::Lt: return b_.CreateFCmpOLT(l, r);
        case BinOp::Le: return b_.CreateFCmpOLE(l, r);
        case BinOp::Gt: return b_.CreateFCmpOGT(l, r);
        case BinOp::Ge: return b_.CreateFCmpOGE(l, r);
        case BinOp::Eq: return b_.CreateFCmpOEQ(l, r);
        case BinOp::Ne: return b_.CreateFCmpUNE(l, r);
        default: break;
      }
      return nullptr;
    }

    if (op == BinOp::Div || op == BinOp::Mod) {
      guard(b_.CreateICmpEQ(r, llvm::ConstantInt::get(r->getType(), 0)), loc,
            op == BinOp::Div ? "division by zero" : "modulo by zero");
    }

    switch (op) {
      case BinOp::Add: return b_.CreateAdd(l, r);
      case BinOp::Sub: return b_.CreateSub(l, r);
      case BinOp::Mul: return b_.CreateMul(l, r);
      case BinOp::Div: return b_.CreateSDiv(l, r);
      case BinOp::Mod: return b_.CreateSRem(l, r);
      case BinOp::Lt: return b_.CreateICmpSLT(l, r);
      case BinOp::Le: return b_.CreateICmpSLE(l, r);
      case BinOp::Gt: return b_.CreateICmpSGT(l, r);
      case BinOp::Ge: return b_.CreateICmpSGE(l, r);
      case BinOp::Eq: return b_.CreateICmpEQ(l, r);
      case BinOp::Ne: return b_.CreateICmpNE(l, r);
      default: break;
    }
    return nullptr;
  }

  llvm::Value *genCall(const Call &n) {
    if (n.callee == "print") return genPrint(*n.args[0]);
    if (n.callee == "len") return b_.CreateExtractValue(genExpr(*n.args[0]), 1, "len");
    std::vector<llvm::Value *> args;
    for (auto &a : n.args) args.push_back(genExpr(*a));
    return b_.CreateCall(mod_->getFunction(mangle(n.callee)), args);
  }

  // printf's conversion for a scalar, and the value to pass for it.
  std::pair<const char *, llvm::Value *> formatScalar(Kind k, llvm::Value *v) {
    switch (k) {
      case Kind::Int: return {"%lld", v};
      case Kind::Float: return {"%g", v};
      case Kind::Bool:
        return {"%s", b_.CreateSelect(v, b_.CreateGlobalStringPtr("true", "str.true"),
                                      b_.CreateGlobalStringPtr("false", "str.false"))};
      default: return {"%s", v};
    }
  }

  void printf(const std::string &fmt, std::vector<llvm::Value *> args = {}) {
    args.insert(args.begin(), b_.CreateGlobalStringPtr(fmt, "fmt"));
    b_.CreateCall(printf_, args);
  }

  // Values print with their unit, e.g. "9.8 m/s^2"; arrays print as "[1, 2, 3] m".
  llvm::Value *genPrint(const Expr &arg) {
    llvm::Value *v = genExpr(arg);
    const Type &t = arg.type;
    std::string suffix = t.unit.dimensionless() ? "\n" : " " + t.unit.str() + "\n";
    if (!t.array) {
      auto [conv, val] = formatScalar(t.kind, v);
      printf(std::string(conv) + suffix, {val});
      return nullptr;
    }
    llvm::Type *ety = lowerScalar(t.kind);
    llvm::Value *data = b_.CreateExtractValue(v, 0);
    llvm::Value *len = b_.CreateExtractValue(v, 1);
    printf("[");
    genCountedLoop(b_.getInt64(0), len, "print", [&](llvm::Value *i) {
      auto [conv, val] = formatScalar(t.kind, b_.CreateLoad(ety, b_.CreateGEP(ety, data, i)));
      llvm::Value *sep = b_.CreateSelect(b_.CreateICmpEQ(i, b_.getInt64(0)), b_.CreateGlobalStringPtr("", "sep0"),
                                         b_.CreateGlobalStringPtr(", ", "sep"));
      printf(std::string("%s") + conv, {sep, val});
    });
    printf("]" + suffix);
    return nullptr;
  }

  llvm::Value *genCast(const Cast &n) {
    llvm::Value *v = genExpr(*n.operand);
    Kind from = n.operand->type.kind, to = n.target.kind;
    if (from == to) return v;  // same representation (possibly a unit change)
    if (to == Kind::Bool) {
      if (from == Kind::Float) return b_.CreateFCmpUNE(v, llvm::ConstantFP::get(b_.getDoubleTy(), 0.0));
      return b_.CreateICmpNE(v, b_.getInt64(0));
    }
    if (to == Kind::Int) {
      if (from == Kind::Float) return b_.CreateFPToSI(v, b_.getInt64Ty());
      return b_.CreateZExt(v, b_.getInt64Ty());  // bool -> int
    }
    // to float
    if (from == Kind::Int) return b_.CreateSIToFP(v, b_.getDoubleTy());
    return b_.CreateUIToFP(v, b_.getDoubleTy());  // bool -> float
  }
};

}  // namespace

std::unique_ptr<llvm::Module> generateIR(const Program &prog, llvm::LLVMContext &ctx,
                                         const std::string &moduleName) {
  auto mod = CodeGen(ctx, moduleName).run(prog);
  if (llvm::verifyModule(*mod, &llvm::errs())) {
    llvm::errs() << "internal compiler error: generated invalid LLVM IR\n";
    return nullptr;
  }
  return mod;
}

void optimize(llvm::Module &mod, int level) {
  if (level <= 0) return;
  llvm::LoopAnalysisManager lam;
  llvm::FunctionAnalysisManager fam;
  llvm::CGSCCAnalysisManager cgam;
  llvm::ModuleAnalysisManager mam;
  llvm::PassBuilder pb;
  pb.registerModuleAnalyses(mam);
  pb.registerCGSCCAnalyses(cgam);
  pb.registerFunctionAnalyses(fam);
  pb.registerLoopAnalyses(lam);
  pb.crossRegisterProxies(lam, fam, cgam, mam);
  llvm::OptimizationLevel opt = level == 1   ? llvm::OptimizationLevel::O1
                                : level == 2 ? llvm::OptimizationLevel::O2
                                             : llvm::OptimizationLevel::O3;
  pb.buildPerModuleDefaultPipeline(opt).run(mod, mam);
}

bool emitObject(llvm::Module &mod, const std::string &path, int optLevel, std::string &err) {
  llvm::InitializeNativeTarget();
  llvm::InitializeNativeTargetAsmPrinter();

  std::string triple = llvm::sys::getDefaultTargetTriple();
  const llvm::Target *target = llvm::TargetRegistry::lookupTarget(triple, err);
  if (!target) return false;

  llvm::TargetOptions options;
  auto level = optLevel <= 0 ? llvm::CodeGenOptLevel::None
               : optLevel == 1 ? llvm::CodeGenOptLevel::Less
               : optLevel == 2 ? llvm::CodeGenOptLevel::Default
                               : llvm::CodeGenOptLevel::Aggressive;
  std::unique_ptr<llvm::TargetMachine> tm(
      target->createTargetMachine(triple, "generic", "", options, llvm::Reloc::PIC_, std::nullopt, level));
  mod.setDataLayout(tm->createDataLayout());
  mod.setTargetTriple(triple);

  std::error_code ec;
  llvm::raw_fd_ostream out(path, ec, llvm::sys::fs::OF_None);
  if (ec) {
    err = ec.message();
    return false;
  }
  llvm::legacy::PassManager pm;
  if (tm->addPassesToEmitFile(pm, out, nullptr, llvm::CodeGenFileType::ObjectFile)) {
    err = "target cannot emit object files";
    return false;
  }
  pm.run(mod);
  out.flush();
  return true;
}

}  // namespace nova
