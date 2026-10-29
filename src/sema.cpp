#include "sema.h"

#include <functional>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "diagnostics.h"

namespace nova {

namespace {

bool isUnitVar(const std::string &name) { return !name.empty() && name[0] == '\''; }

struct Signature {
  std::vector<Type> params;
  Type ret;
  std::set<std::string> unitVars;  // 'u, 'v, ... that the signature is polymorphic over
};

struct VarInfo {
  Type type;
  bool isMutable;
};

// A `for i in 0..len(a)` loop where `a`'s length provably can't change: inside the
// body, a[i] can never be out of bounds, so its runtime check is unnecessary.
struct SafeRange {
  const VarInfo *index;
  const VarInfo *array;
};

class Analyzer {
 public:
  void run(Program &prog) {
    for (auto &u : prog.units) declareUnit(u);
    declareStructs(prog);
    declareBuiltins();

    for (auto &f : prog.functions) {
      if (builtins_.count(f->name)) {
        diag().error(f->loc, "'" + f->name + "' is a builtin and cannot be redefined");
        continue;
      }
      // Library builtins (sqrt, abs) may be shadowed by a user definition, once.
      if (funcs_.count(f->name) && !library_.erase(f->name)) {
        diag().error(f->loc, "redefinition of function '" + f->name + "'");
        continue;
      }
      funcs_[f->name] = declareSignature(*f);
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
  std::map<std::string, StructDecl *> structs_;
  std::map<std::string, Signature> funcs_;
  std::set<std::string> builtins_;  // reserved names: print, len
  std::set<std::string> library_;   // builtins a program may redefine: sqrt, abs
  std::vector<std::map<std::string, VarInfo>> scopes_;
  std::vector<SafeRange> safeRanges_;
  std::set<std::string> unitVarsInScope_;  // unit variables of the function being checked
  Type currentReturn_;
  int loopDepth_ = 0;
  int instantiations_ = 0;

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
  // Unit variables are kept symbolically and must be in scope.
  Unit resolveUnit(const Unit &written, Loc loc) {
    Unit out;
    for (auto &[name, exp] : written.dims) {
      Unit factor;
      if (isUnitVar(name)) {
        if (!unitVarsInScope_.count(name)) {
          diag().error(loc, "unit variable " + name + " is not bound by the enclosing function's parameters");
          continue;
        }
        factor.dims[name] = 1;
      } else {
        auto it = units_.find(name);
        if (it == units_.end()) {
          diag().error(loc, "unknown unit '" + name + "' (declare it with 'unit " + name + ";')");
          continue;
        }
        factor = it->second;
      }
      out = out * factor.pow(exp);
    }
    return out;
  }

  void resolveType(Type &t, Loc loc) {
    if (t.kind == Kind::Struct && !structs_.count(t.structName)) {
      diag().error(loc, "unknown type '" + t.structName + "'");
      t = Type();
      return;
    }
    if (t.unit.dimensionless()) return;
    if (t.kind != Kind::Int && t.kind != Kind::Float) {
      diag().error(loc, "units can only be attached to int or float, not " +
                            (t.kind == Kind::Struct ? t.structName : std::string(kindName(t.kind))));
      t.unit = Unit();
      return;
    }
    t.unit = resolveUnit(t.unit, loc);
  }

  static void collectUnitVars(const Type &t, std::set<std::string> &out) {
    for (auto &[name, exp] : t.unit.dims)
      if (isUnitVar(name)) out.insert(name);
  }

  // ---------------------------------------------------------------- structs

  void declareStructs(Program &prog) {
    for (auto &s : prog.structs) {
      if (structs_.count(s.name)) {
        diag().error(s.loc, "redefinition of struct '" + s.name + "'");
        continue;
      }
      structs_[s.name] = &s;
    }
    for (auto &s : prog.structs) {
      std::set<std::string> seen;
      for (auto &f : s.fields) {
        if (!seen.insert(f.name).second) diag().error(f.loc, "duplicate field '" + f.name + "' in '" + s.name + "'");
        resolveType(f.type, f.loc);
        if (f.type.kind == Kind::Void) diag().error(f.loc, "field '" + f.name + "' cannot have type void");
      }
    }
    // A struct that contains itself by value (directly or through other structs)
    // would have infinite size. Containing it through an array is fine: that is a pointer.
    std::map<std::string, int> state;  // 0 = unvisited, 1 = in progress, 2 = done
    std::function<bool(const std::string &)> visit = [&](const std::string &name) {
      if (state[name] == 2) return false;
      if (state[name] == 1) return true;
      state[name] = 1;
      for (auto &f : structs_[name]->fields) {
        if (f.type.kind == Kind::Struct && !f.type.array && structs_.count(f.type.structName) &&
            visit(f.type.structName)) {
          state[name] = 2;
          return true;
        }
      }
      state[name] = 2;
      return false;
    };
    for (auto &[name, decl] : structs_) {
      state.clear();
      if (visit(name))
        diag().error(decl->loc, "struct '" + name + "' contains itself and would have infinite size "
                                "(use an array field to hold a variable number of them)");
    }
  }

  const FieldDecl *findField(const std::string &structName, const std::string &field, int &index) {
    auto it = structs_.find(structName);
    if (it == structs_.end()) return nullptr;
    for (size_t i = 0; i < it->second->fields.size(); ++i) {
      if (it->second->fields[i].name == field) {
        index = (int)i;
        return &it->second->fields[i];
      }
    }
    return nullptr;
  }

  // ------------------------------------------------------------- signatures

  void declareBuiltins() {
    builtins_ = {"print", "len"};
    library_ = {"sqrt", "abs"};
    // sqrt: float<'u^2> -> float<'u>, so sqrt of an area is a length and
    // sqrt of a plain length is a compile-time error.
    Signature sqrtSig;
    Type sq(Kind::Float);
    sq.unit.dims["'u"] = 2;
    Type root(Kind::Float);
    root.unit.dims["'u"] = 1;
    sqrtSig.params = {sq};
    sqrtSig.ret = root;
    sqrtSig.unitVars = {"'u"};
    funcs_["sqrt"] = sqrtSig;

    Signature absSig;
    absSig.params = {root};
    absSig.ret = root;
    absSig.unitVars = {"'u"};
    funcs_["abs"] = absSig;
  }

  Signature declareSignature(Function &f) {
    // Every unit variable mentioned by a parameter is a generic parameter of the function.
    Signature sig;
    for (auto &p : f.params) collectUnitVars(p.type, sig.unitVars);
    unitVarsInScope_ = sig.unitVars;

    for (auto &p : f.params) {
      resolveType(p.type, p.loc);
      if (p.type.kind == Kind::Void) diag().error(p.loc, "parameter '" + p.name + "' cannot have type void");
      sig.params.push_back(p.type);
    }
    std::set<std::string> retVars;
    collectUnitVars(f.returnType, retVars);
    for (auto &v : retVars)
      if (!sig.unitVars.count(v))
        diag().error(f.loc, "unit variable " + v + " appears in the return type of '" + f.name +
                                "' but in no parameter, so it can never be inferred");
    unitVarsInScope_.insert(retVars.begin(), retVars.end());  // already reported above
    resolveType(f.returnType, f.loc);
    sig.ret = f.returnType;
    unitVarsInScope_.clear();
    return sig;
  }

  // ----------------------------------------------------------------- scopes

  void push() { scopes_.emplace_back(); }
  void pop() { scopes_.pop_back(); }

  const VarInfo *declare(const std::string &name, VarInfo info, Loc loc) {
    auto &scope = scopes_.back();
    if (scope.count(name)) {
      diag().error(loc, "redeclaration of '" + name + "' in the same scope");
      return nullptr;
    }
    return &(scope[name] = info);
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
    // Inside the body, the function's unit variables are opaque base units: 'u * 'u
    // simplifies to 'u^2 by the same exponent arithmetic as m * m.
    unitVarsInScope_.clear();
    for (auto &p : f.params) collectUnitVars(p.type, unitVarsInScope_);
    push();
    for (auto &p : f.params) declare(p.name, {p.type, p.isMutable}, p.loc);
    bool returns = checkBlock(*f.body, /*newScope=*/false);
    pop();
    unitVarsInScope_.clear();
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
      const VarInfo *array = provablySafeRange(*fs);
      push();
      const VarInfo *index = declare(fs->var, {Kind::Int, false}, fs->loc);
      if (array && index) safeRanges_.push_back({index, array});
      ++loopDepth_;
      checkStmt(*fs->body);
      --loopDepth_;
      if (array && index) safeRanges_.pop_back();
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

  // For `for i in <non-negative constant>..len(a)`, returns a's VarInfo if a[i] is
  // provably in bounds throughout the loop body. An array's length lives in the fat
  // pointer held by its binding and never changes, so writing elements (a[j] = v) is
  // harmless; only rebinding the whole variable (a = other) could shrink it. So `a`
  // qualifies if it is immutable, or if the body never assigns to `a` itself.
  const VarInfo *provablySafeRange(const ForStmt &fs) {
    auto *end = dynamic_cast<const Call *>(fs.end.get());
    if (!nonNegative(*fs.start) || !end || end->callee != "len" || end->args.size() != 1) return nullptr;
    auto *arr = dynamic_cast<const VarRef *>(end->args[0].get());
    if (!arr) return nullptr;
    const VarInfo *info = lookup(arr->name);
    if (!info || !info->type.array) return nullptr;
    if (info->isMutable && rebinds(*fs.body, arr->name)) return nullptr;
    return info;
  }

  // A tiny sign analysis for range starts: a non-negative literal, or `i + c` where `i`
  // is the index of an enclosing safe loop (so 0 <= i < len(a) < 2^60, because no array
  // can be that large) and c is a small non-negative literal, which therefore can't overflow.
  bool nonNegative(const Expr &e) {
    if (auto *lit = dynamic_cast<const IntLit *>(&e)) return lit->value >= 0;
    if (auto *v = dynamic_cast<const VarRef *>(&e)) return boundedIndex(v->name);
    if (auto *b = dynamic_cast<const Binary *>(&e)) {
      if (b->op != BinOp::Add) return false;
      auto smallConst = [](const Expr &x) {
        auto *lit = dynamic_cast<const IntLit *>(&x);
        return lit && lit->value >= 0 && lit->value < (1LL << 32);
      };
      auto *l = dynamic_cast<const VarRef *>(b->lhs.get());
      auto *r = dynamic_cast<const VarRef *>(b->rhs.get());
      return (l && boundedIndex(l->name) && smallConst(*b->rhs)) ||
             (r && boundedIndex(r->name) && smallConst(*b->lhs));
    }
    return false;
  }

  bool boundedIndex(const std::string &name) {
    const VarInfo *info = lookup(name);
    for (auto &r : safeRanges_)
      if (r.index == info) return true;
    return false;
  }

  // Conservative: any `name = ...` in the body counts, even if it targets a shadowing variable.
  static bool rebinds(const Stmt &s, const std::string &name) {
    if (auto *as = dynamic_cast<const AssignStmt *>(&s)) {
      auto *v = dynamic_cast<const VarRef *>(as->target.get());
      return v && v->name == name;
    }
    if (auto *b = dynamic_cast<const Block *>(&s)) {
      for (auto &st : b->stmts)
        if (rebinds(*st, name)) return true;
      return false;
    }
    if (auto *is = dynamic_cast<const IfStmt *>(&s))
      return rebinds(*is->thenBranch, name) || (is->elseBranch && rebinds(*is->elseBranch, name));
    if (auto *ws = dynamic_cast<const WhileStmt *>(&s)) return rebinds(*ws->body, name);
    if (auto *fs = dynamic_cast<const ForStmt *>(&s)) return rebinds(*fs->body, name);
    if (auto *fe = dynamic_cast<const ForEachStmt *>(&s)) return rebinds(*fe->body, name);
    return false;
  }

  // Walks an assignment target (x, x[i], x.f, x[i].f.g, ...) down to its root variable.
  bool assignmentRoot(Expr *target, std::string &rootName) {
    while (true) {
      if (auto *v = dynamic_cast<VarRef *>(target)) {
        rootName = v->name;
        return true;
      }
      if (auto *ix = dynamic_cast<Index *>(target)) {
        target = ix->base.get();
      } else if (auto *fa = dynamic_cast<FieldAccess *>(target)) {
        target = fa->base.get();
      } else {
        return false;
      }
    }
  }

  void checkAssign(AssignStmt &as) {
    Type value = checkExpr(*as.value);

    std::string rootName;
    if (!assignmentRoot(as.target.get(), rootName)) {
      diag().error(as.loc, "invalid assignment target");
      return;
    }
    const VarInfo *root = lookup(rootName);
    if (!root) {
      diag().error(as.loc, "assignment to undeclared variable '" + rootName + "'");
      return;
    }
    Type target = checkExpr(*as.target);

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
      if (l.sameShape(r) && l.unit != r.unit) msg += " (units do not match)";
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
        if (l != r || !l.isValue() || l.kind == Kind::String || l.kind == Kind::Struct) return mismatch();
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
      if (!elem.isValue()) {
        diag().error(a->loc, "array elements cannot have type " + elem.str());
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
      if (!elem.isValue()) {
        diag().error(a->value->loc, "array elements cannot have type " + elem.str());
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
      ix->checked = !inSafeRange(*ix);
      return base.element();
    }

    if (auto *fa = dynamic_cast<FieldAccess *>(&e)) {
      Type base = checkExpr(*fa->base);
      if (!base.known()) return Type();
      if (base.kind != Kind::Struct || base.array) {
        diag().error(fa->loc, "cannot access field '" + fa->field + "' on a value of type " + base.str());
        return Type();
      }
      const FieldDecl *field = findField(base.structName, fa->field, fa->fieldIndex);
      if (!field) {
        diag().error(fa->loc, "struct '" + base.structName + "' has no field '" + fa->field + "'");
        return Type();
      }
      return field->type;
    }

    if (auto *sl = dynamic_cast<StructLit *>(&e)) return checkStructLit(*sl);

    return Type();
  }

  bool inSafeRange(const Index &ix) {
    auto *idx = dynamic_cast<const VarRef *>(ix.index.get());
    auto *arr = dynamic_cast<const VarRef *>(ix.base.get());
    if (!idx || !arr) return false;
    // Resolve through the current scopes so shadowed names don't qualify.
    const VarInfo *i = lookup(idx->name);
    const VarInfo *a = lookup(arr->name);
    for (auto &r : safeRanges_)
      if (r.index == i && r.array == a) return true;
    return false;
  }

  Type checkStructLit(StructLit &sl) {
    auto it = structs_.find(sl.name);
    if (it == structs_.end()) {
      for (auto &f : sl.fields) checkExpr(*f.value);
      diag().error(sl.loc, "unknown struct '" + sl.name + "'");
      return Type();
    }
    const StructDecl &decl = *it->second;
    std::set<std::string> given;
    for (auto &f : sl.fields) {
      Type t = checkExpr(*f.value);
      if (!given.insert(f.name).second) {
        diag().error(f.loc, "field '" + f.name + "' is given more than once");
        continue;
      }
      int index;
      const FieldDecl *field = findField(sl.name, f.name, index);
      if (!field) {
        diag().error(f.loc, "struct '" + sl.name + "' has no field '" + f.name + "'");
        continue;
      }
      if (t.known() && field->type.known() && t != field->type)
        diag().error(f.value->loc, "field '" + f.name + "' of '" + sl.name + "' has type " + field->type.str() +
                                       ", got " + t.str());
    }
    for (auto &f : decl.fields)
      if (!given.count(f.name)) diag().error(sl.loc, "missing field '" + f.name + "' in '" + sl.name + "' literal");
    Type t(Kind::Struct);
    t.structName = sl.name;
    return t;
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
      return sig.unitVars.empty() ? sig.ret : Type();
    }
    return instantiate(c, sig, argTypes);
  }

  // ------------------------------------------------------- unit inference

  // Replaces unit variables with their solutions.
  static Unit substitute(const Unit &u, const std::map<std::string, Unit> &subst) {
    Unit out;
    for (auto &[name, exp] : u.dims) {
      auto it = subst.find(name);
      Unit factor;
      if (it != subst.end()) {
        factor = it->second;
      } else {
        factor.dims[name] = 1;
      }
      out = out * factor.pow(exp);
    }
    return out;
  }

  // Checks a call against a (possibly unit-polymorphic) signature.
  //
  // Units form a free abelian group, so each argument gives an equation
  //   param_unit(vars) = arg_unit
  // over integer exponent vectors. We solve by elimination: whenever an equation has
  // exactly one unsolved variable 'u with exponent k, then 'u = (arg / rest)^(1/k),
  // which exists only if every exponent is divisible by k. This repeats until a fixed
  // point. Since units are erased before code generation, the solution is only used
  // for checking; the same machine code serves every instantiation.
  Type instantiate(Call &c, const Signature &sig, const std::vector<Type> &args) {
    // Rename the callee's unit variables apart from the caller's ('u -> 'u#7), so a
    // generic function calling another generic function can't confuse the two.
    int id = ++instantiations_;
    std::map<std::string, std::string> fresh;
    for (auto &v : sig.unitVars) fresh[v] = v + "#" + std::to_string(id);
    auto rename = [&](Unit u) {
      Unit out;
      for (auto &[name, exp] : u.dims) out.dims[fresh.count(name) ? fresh[name] : name] = exp;
      return out;
    };
    std::set<std::string> vars;
    for (auto &[orig, renamed] : fresh) vars.insert(renamed);

    struct Equation {
      size_t arg;
      Unit param;
      Unit actual;
      bool done = false;
    };
    std::vector<Equation> eqs;
    bool ok = true;
    for (size_t i = 0; i < args.size(); ++i) {
      if (!args[i].known()) {
        ok = false;
        continue;
      }
      if (!args[i].sameShape(sig.params[i])) {
        diag().error(c.args[i]->loc, "argument " + std::to_string(i + 1) + " of '" + c.callee + "' expects " +
                                         sig.params[i].str() + ", got " + args[i].str());
        ok = false;
        continue;
      }
      eqs.push_back({i, rename(sig.params[i].unit), args[i].unit});
    }

    std::map<std::string, Unit> subst;
    auto argError = [&](const Equation &eq, const std::string &detail) {
      diag().error(c.args[eq.arg]->loc, "argument " + std::to_string(eq.arg + 1) + " of '" + c.callee +
                                            "' expects " + sig.params[eq.arg].str() + ", got " +
                                            args[eq.arg].str() + detail);
      ok = false;
    };

    for (bool progress = true; progress;) {
      progress = false;
      for (auto &eq : eqs) {
        if (eq.done) continue;
        Unit p = substitute(eq.param, subst);
        std::vector<std::pair<std::string, int>> unknown;
        for (auto &[name, exp] : p.dims)
          if (vars.count(name)) unknown.push_back({name, exp});

        if (unknown.empty()) {
          eq.done = true;
          if (p != eq.actual) argError(eq, "");
          progress = true;
        } else if (unknown.size() == 1) {
          auto [var, k] = unknown[0];
          Unit rest = p;
          rest.dims.erase(var);
          Unit need = eq.actual / rest;
          Unit solution;
          bool divisible = true;
          for (auto &[name, exp] : need.dims) {
            if (exp % k != 0) divisible = false;
            solution.dims[name] = exp / k;
          }
          eq.done = true;
          progress = true;
          if (!divisible) {
            std::string what = k == 2 ? "square root" : k == 3 ? "cube root" : std::to_string(k) + "th root";
            argError(eq, " (" + need.str() + " has no exact " + what + ")");
            continue;
          }
          subst[var] = solution;
        }
      }
    }
    for (auto &eq : eqs) {
      if (!eq.done) {
        diag().error(c.loc, "cannot infer the unit variables of '" + c.callee +
                                "': a parameter mixes several unknown unit variables");
        return Type();
      }
    }
    if (!ok) return Type();
    Type ret = sig.ret;
    ret.unit = substitute(rename(ret.unit), subst);
    return ret;
  }
};

}  // namespace

void analyze(Program &prog) { Analyzer().run(prog); }

}  // namespace nova
