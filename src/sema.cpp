#include "sema.h"

#include <map>
#include <string>
#include <vector>

#include "diagnostics.h"

namespace nova {

namespace {

struct Signature {
  std::vector<Type> params;
  Type ret;
};

struct VarInfo {
  Type type;
  bool isMutable;
};

const char *const kBuiltins[] = {"print", "len"};

class Analyzer {
 public:
  void run(Program &prog) {
    for (auto &u : prog.units) declareUnit(u);

    for (auto &f : prog.functions) {
      bool builtin = false;
      for (const char *b : kBuiltins)
        if (f->name == b) builtin = true;
      if (builtin) {
        diag().error(f->loc, "'" + f->name + "' is a builtin and cannot be redefined");
        continue;
      }
      if (funcs_.count(f->name)) {
        diag().error(f->loc, "redefinition of function '" + f->name + "'");
        continue;
      }
      Signature sig;
      for (auto &p : f->params) {
        resolveType(p.type, p.loc);
        if (p.type.kind == Kind::Void) diag().error(p.loc, "parameter '" + p.name + "' cannot have type void");
        sig.params.push_back(p.type);
      }
      resolveType(f->returnType, f->loc);
      sig.ret = f->returnType;
      funcs_[f->name] = sig;
    }

    auto main = funcs_.find("main");
    if (main == funcs_.end()) {
      diag().error({1, 1}, "program has no 'main' function");
    } else if (!main->second.params.empty() ||
               !(main->second.ret.is(Kind::Int) || main->second.ret.is(Kind::Void))) {
      Loc loc{1, 1};
      for (auto &f : prog.functions)
        if (f->name == "main") loc = f->loc;
      diag().error(loc, "'main' must take no parameters and return int or void");
    }

    for (auto &f : prog.functions) checkFunction(*f);
  }

 private:
  std::map<std::string, Unit> units_;  // unit name -> expansion in base units
  std::map<std::string, Signature> funcs_;
  std::vector<std::map<std::string, VarInfo>> scopes_;
  Type currentReturn_;
  int loopDepth_ = 0;

  // ------------------------------------------------------------------ units

  void declareUnit(UnitDecl &u) {
    if (units_.count(u.name)) {
      diag().error(u.loc, "redefinition of unit '" + u.name + "'");
      return;
    }
    if (u.alias) {
      units_[u.name] = resolveUnit(*u.alias, u.loc);
    } else {
      Unit base;
      base.dims[u.name] = 1;
      units_[u.name] = base;
    }
  }

  // Rewrites a unit as written (which may mention aliases) into base units.
  Unit resolveUnit(const Unit &written, Loc loc) {
    Unit out;
    for (auto &[name, exp] : written.dims) {
      auto it = units_.find(name);
      if (it == units_.end()) {
        diag().error(loc, "unknown unit '" + name + "' (declare it with 'unit " + name + ";')");
        continue;
      }
      out = out * it->second.pow(exp);
    }
    return out;
  }

  void resolveType(Type &t, Loc loc) {
    if (t.unit.dimensionless()) return;
    if (t.kind != Kind::Int && t.kind != Kind::Float) {
      diag().error(loc, std::string("units can only be attached to int or float, not ") + kindName(t.kind));
      t.unit = Unit();
      return;
    }
    t.unit = resolveUnit(t.unit, loc);
  }

  // ----------------------------------------------------------------- scopes

  void push() { scopes_.emplace_back(); }
  void pop() { scopes_.pop_back(); }

  void declare(const std::string &name, VarInfo info, Loc loc) {
    auto &scope = scopes_.back();
    if (scope.count(name)) {
      diag().error(loc, "redeclaration of '" + name + "' in the same scope");
      return;
    }
    scope[name] = info;
  }

  const VarInfo *lookup(const std::string &name) const {
    for (auto it = scopes_.rbegin(); it != scopes_.rend(); ++it) {
      auto found = it->find(name);
      if (found != it->end()) return &found->second;
    }
    return nullptr;
  }

  // ------------------------------------------------------------- statements

  void checkFunction(Function &f) {
    currentReturn_ = f.returnType;
    loopDepth_ = 0;
    push();
    for (auto &p : f.params) declare(p.name, {p.type, p.isMutable}, p.loc);
    bool returns = checkBlock(*f.body, /*newScope=*/false);
    pop();
    if (f.returnType.kind != Kind::Void && !returns)
      diag().error(f.loc, "function '" + f.name + "' may finish without returning a value of type " +
                              f.returnType.str());
  }

  // Each check* for statements returns true if the statement always returns.
  bool checkBlock(Block &b, bool newScope = true) {
    if (newScope) push();
    bool returns = false;
    for (auto &s : b.stmts)
      if (checkStmt(*s)) returns = true;
    if (newScope) pop();
    return returns;
  }

  bool checkStmt(Stmt &s) {
    if (auto *b = dynamic_cast<Block *>(&s)) return checkBlock(*b);

    if (auto *let = dynamic_cast<LetStmt *>(&s)) {
      Type t = checkExpr(*let->init);
      if (let->declared.known()) resolveType(let->declared, let->loc);
      if (let->declared.kind == Kind::Void) {
        diag().error(let->loc, "variable '" + let->name + "' cannot have type void");
      } else if (t.kind == Kind::Void) {
        diag().error(let->init->loc, "cannot initialize '" + let->name + "' with a void expression");
        t = Type();
      } else if (let->declared.known() && t.known() && t != let->declared) {
        diag().error(let->init->loc, "cannot initialize '" + let->name + "' of type " + let->declared.str() +
                                         " with a value of type " + t.str());
      }
      // Type inference: an undeclared type takes the type of the initializer.
      if (!let->declared.known()) let->declared = t;
      declare(let->name, {let->declared, let->isMutable}, let->loc);
      return false;
    }

    if (auto *as = dynamic_cast<AssignStmt *>(&s)) {
      checkAssign(*as);
      return false;
    }

    if (auto *es = dynamic_cast<ExprStmt *>(&s)) {
      checkExpr(*es->expr);
      return false;
    }

    if (auto *ret = dynamic_cast<ReturnStmt *>(&s)) {
      if (!ret->value) {
        if (currentReturn_.kind != Kind::Void)
          diag().error(ret->loc, "missing return value of type " + currentReturn_.str());
      } else {
        Type t = checkExpr(*ret->value);
        if (currentReturn_.kind == Kind::Void)
          diag().error(ret->loc, "void function cannot return a value");
        else if (t.known() && t != currentReturn_)
          diag().error(ret->value->loc, "returning " + t.str() + " from a function declared to return " +
                                            currentReturn_.str());
      }
      return true;
    }

    if (auto *is = dynamic_cast<IfStmt *>(&s)) {
      expectBool(*is->cond, "if condition");
      bool thenReturns = checkStmt(*is->thenBranch);
      bool elseReturns = is->elseBranch ? checkStmt(*is->elseBranch) : false;
      return thenReturns && elseReturns;
    }

    if (auto *ws = dynamic_cast<WhileStmt *>(&s)) {
      expectBool(*ws->cond, "while condition");
      ++loopDepth_;
      checkStmt(*ws->body);
      --loopDepth_;
      return false;
    }

    if (auto *fs = dynamic_cast<ForStmt *>(&s)) {
      Type st = checkExpr(*fs->start);
      Type et = checkExpr(*fs->end);
      if (st.known() && !st.is(Kind::Int))
        diag().error(fs->start->loc, "range start must be int, got " + st.str());
      if (et.known() && !et.is(Kind::Int))
        diag().error(fs->end->loc, "range end must be int, got " + et.str());
      push();
      declare(fs->var, {Kind::Int, false}, fs->loc);
      ++loopDepth_;
      checkStmt(*fs->body);
      --loopDepth_;
      pop();
      return false;
    }

    if (auto *fe = dynamic_cast<ForEachStmt *>(&s)) {
      Type it = checkExpr(*fe->iterable);
      Type elem;
      if (it.known()) {
        if (it.array)
          elem = it.element();
        else
          diag().error(fe->iterable->loc, "can only iterate over an array or a range, got " + it.str());
      }
      push();
      declare(fe->var, {elem, false}, fe->loc);
      ++loopDepth_;
      checkStmt(*fe->body);
      --loopDepth_;
      pop();
      return false;
    }

    if (dynamic_cast<BreakStmt *>(&s)) {
      if (loopDepth_ == 0) diag().error(s.loc, "'break' outside of a loop");
      return false;
    }

    if (dynamic_cast<ContinueStmt *>(&s)) {
      if (loopDepth_ == 0) diag().error(s.loc, "'continue' outside of a loop");
      return false;
    }

    return false;
  }

  void checkAssign(AssignStmt &as) {
    Type value = checkExpr(*as.value);

    // Find the variable being written: `x = ...` or `x[i] = ...`.
    Type target;
    const VarInfo *root = nullptr;
    std::string rootName;
    if (auto *v = dynamic_cast<VarRef *>(as.target.get())) {
      rootName = v->name;
      root = lookup(v->name);
      if (!root) {
        diag().error(as.loc, "assignment to undeclared variable '" + v->name + "'");
        return;
      }
      target = v->type = root->type;
    } else if (auto *ix = dynamic_cast<Index *>(as.target.get())) {
      auto *base = dynamic_cast<VarRef *>(ix->base.get());
      if (!base) {
        diag().error(as.loc, "invalid assignment target");
        return;
      }
      rootName = base->name;
      root = lookup(base->name);
      target = checkExpr(*ix);
      if (!root) return;  // already reported as undeclared
    } else {
      diag().error(as.loc, "invalid assignment target");
      return;
    }

    if (!root->isMutable) {
      diag().error(as.loc, "cannot assign to immutable variable '" + rootName +
                               "' (declare it with 'var' to make it mutable)");
      return;
    }
    if (!target.known() || !value.known()) return;

    if (as.compound) {
      Type result = binaryType(as.op, target, value, as.loc, as.value.get());
      if (result.known() && result != target)
        diag().error(as.value->loc, std::string("'") + binOpName(as.op) + "=' would change the type of '" +
                                        rootName + "' from " + target.str() + " to " + result.str());
    } else if (value != target) {
      diag().error(as.value->loc, "cannot assign a value of type " + value.str() + " to '" + rootName +
                                      "' of type " + target.str());
    }
  }

  void expectBool(Expr &e, const char *what) {
    Type t = checkExpr(e);
    if (t.known() && !t.is(Kind::Bool)) diag().error(e.loc, std::string(what) + " must be bool, got " + t.str());
  }

  // ------------------------------------------------------------ expressions

  Type checkExpr(Expr &e) {
    e.type = inferExpr(e);
    return e.type;
  }

  // Shared by binary expressions and compound assignment.
  Type binaryType(BinOp op, const Type &l, const Type &r, Loc loc, const Expr *rhs) {
    auto mismatch = [&]() {
      std::string msg = std::string("operator '") + binOpName(op) + "' cannot be applied to " + l.str() +
                        " and " + r.str();
      if (l.kind == r.kind && l.array == r.array && l.unit != r.unit) msg += " (units do not match)";
      diag().error(loc, msg);
      return Type();
    };
    bool sameNumeric = l.isNumeric() && r.isNumeric() && l.kind == r.kind;
    switch (op) {
      case BinOp::Add:
      case BinOp::Sub:
        if (!sameNumeric || l.unit != r.unit) return mismatch();
        return l;
      case BinOp::Mul:
      case BinOp::Div: {
        if (!sameNumeric) return mismatch();
        if (op == BinOp::Div && l.kind == Kind::Int) checkConstantZero(rhs);
        Type t = l;
        t.unit = op == BinOp::Mul ? l.unit * r.unit : l.unit / r.unit;
        return t;
      }
      case BinOp::Mod:
        if (!sameNumeric || l.kind != Kind::Int || l.unit != r.unit) return mismatch();
        checkConstantZero(rhs);
        return l;
      case BinOp::Lt:
      case BinOp::Le:
      case BinOp::Gt:
      case BinOp::Ge:
        if (!sameNumeric || l.unit != r.unit) return mismatch();
        return Kind::Bool;
      case BinOp::Eq:
      case BinOp::Ne:
        if (l != r || !l.isScalar() || l.kind == Kind::String) return mismatch();
        return Kind::Bool;
      case BinOp::And:
      case BinOp::Or:
        if (!l.is(Kind::Bool) || !r.is(Kind::Bool)) return mismatch();
        return Kind::Bool;
    }
    return Type();
  }

  void checkConstantZero(const Expr *rhs) {
    if (auto *lit = dynamic_cast<const IntLit *>(rhs))
      if (lit->value == 0) diag().error(rhs->loc, "division by zero");
  }

  Type inferExpr(Expr &e) {
    if (auto *n = dynamic_cast<IntLit *>(&e)) {
      Type t(Kind::Int);
      t.unit = resolveUnit(n->unit, n->loc);
      return t;
    }
    if (auto *n = dynamic_cast<FloatLit *>(&e)) {
      Type t(Kind::Float);
      t.unit = resolveUnit(n->unit, n->loc);
      return t;
    }
    if (dynamic_cast<BoolLit *>(&e)) return Kind::Bool;
    if (dynamic_cast<StringLit *>(&e)) return Kind::String;

    if (auto *v = dynamic_cast<VarRef *>(&e)) {
      const VarInfo *info = lookup(v->name);
      if (!info) {
        diag().error(v->loc, "use of undeclared variable '" + v->name + "'");
        return Type();
      }
      return info->type;
    }

    if (auto *u = dynamic_cast<Unary *>(&e)) {
      Type t = checkExpr(*u->operand);
      if (!t.known()) return t;
      if (u->op == UnOp::Neg) {
        if (!t.isNumeric()) {
          diag().error(u->loc, "unary '-' requires int or float, got " + t.str());
          return Type();
        }
        return t;
      }
      if (!t.is(Kind::Bool)) {
        diag().error(u->loc, "'!' requires bool, got " + t.str());
        return Type();
      }
      return Kind::Bool;
    }

    if (auto *b = dynamic_cast<Binary *>(&e)) {
      Type l = checkExpr(*b->lhs);
      Type r = checkExpr(*b->rhs);
      if (!l.known() || !r.known()) return Type();
      return binaryType(b->op, l, r, b->loc, b->rhs.get());
    }

    if (auto *c = dynamic_cast<Call *>(&e)) return checkCall(*c);

    if (auto *c = dynamic_cast<Cast *>(&e)) {
      Type from = checkExpr(*c->operand);
      resolveType(c->target, c->loc);
      if (!from.known()) return c->target;
      auto castable = [](const Type &t) { return t.isNumeric() || t.is(Kind::Bool); };
      bool ok = from == c->target || (castable(from) && castable(c->target));
      if (!ok) {
        diag().error(c->loc, "cannot cast " + from.str() + " to " + c->target.str());
        return Type();
      }
      return c->target;
    }

    if (auto *a = dynamic_cast<ArrayLit *>(&e)) {
      if (a->elements.empty()) return Type();  // reported by the parser
      Type elem = checkExpr(*a->elements[0]);
      for (size_t i = 1; i < a->elements.size(); ++i) {
        Type t = checkExpr(*a->elements[i]);
        if (elem.known() && t.known() && t != elem) {
          diag().error(a->elements[i]->loc, "array element " + std::to_string(i + 1) + " has type " + t.str() +
                                                ", expected " + elem.str());
          return Type();
        }
      }
      if (!elem.known()) return Type();
      if (!elem.isScalar()) {
        diag().error(a->loc, "array elements must be int, float, bool or string, got " + elem.str());
        return Type();
      }
      return elem.arrayOf();
    }

    if (auto *a = dynamic_cast<ArrayRepeat *>(&e)) {
      Type elem = checkExpr(*a->value);
      Type count = checkExpr(*a->count);
      if (count.known() && !count.is(Kind::Int))
        diag().error(a->count->loc, "array length must be int, got " + count.str());
      if (!elem.known()) return Type();
      if (!elem.isScalar()) {
        diag().error(a->value->loc, "array elements must be int, float, bool or string, got " + elem.str());
        return Type();
      }
      return elem.arrayOf();
    }

    if (auto *ix = dynamic_cast<Index *>(&e)) {
      Type base = checkExpr(*ix->base);
      Type index = checkExpr(*ix->index);
      if (index.known() && !index.is(Kind::Int))
        diag().error(ix->index->loc, "array index must be int, got " + index.str());
      if (!base.known()) return Type();
      if (!base.array) {
        diag().error(ix->loc, "cannot index into a value of type " + base.str());
        return Type();
      }
      return base.element();
    }

    return Type();
  }

  Type checkCall(Call &c) {
    std::vector<Type> argTypes;
    for (auto &a : c.args) argTypes.push_back(checkExpr(*a));

    if (c.callee == "print") {
      if (argTypes.size() != 1)
        diag().error(c.loc, "'print' expects exactly 1 argument, got " + std::to_string(argTypes.size()));
      else if (argTypes[0].kind == Kind::Void)
        diag().error(c.args[0]->loc, "cannot print a void expression");
      return Kind::Void;
    }

    if (c.callee == "len") {
      if (argTypes.size() != 1)
        diag().error(c.loc, "'len' expects exactly 1 argument, got " + std::to_string(argTypes.size()));
      else if (argTypes[0].known() && !argTypes[0].array)
        diag().error(c.args[0]->loc, "'len' expects an array, got " + argTypes[0].str());
      return Kind::Int;
    }

    auto it = funcs_.find(c.callee);
    if (it == funcs_.end()) {
      diag().error(c.loc, "call to undefined function '" + c.callee + "'");
      return Type();
    }
    const Signature &sig = it->second;
    if (sig.params.size() != argTypes.size()) {
      diag().error(c.loc, "function '" + c.callee + "' expects " + std::to_string(sig.params.size()) +
                              " argument(s), got " + std::to_string(argTypes.size()));
      return sig.ret;
    }
    for (size_t i = 0; i < argTypes.size(); ++i) {
      if (argTypes[i].known() && argTypes[i] != sig.params[i])
        diag().error(c.args[i]->loc, "argument " + std::to_string(i + 1) + " of '" + c.callee + "' expects " +
                                         sig.params[i].str() + ", got " + argTypes[i].str());
    }
    return sig.ret;
  }
};

}  // namespace

void analyze(Program &prog) { Analyzer().run(prog); }

}  // namespace nova
