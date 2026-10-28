#include "ast.h"

#include <iostream>

#include "diagnostics.h"

namespace nova {

const char *kindName(Kind k) {
  switch (k) {
    case Kind::Int: return "int";
    case Kind::Float: return "float";
    case Kind::Bool: return "bool";
    case Kind::String: return "string";
    case Kind::Void: return "void";
    case Kind::Unknown: break;
  }
  return "<unknown>";
}

Unit Unit::operator*(const Unit &o) const {
  Unit r = *this;
  for (auto &[name, exp] : o.dims) {
    int &d = r.dims[name];
    d += exp;
    if (d == 0) r.dims.erase(name);
  }
  return r;
}

Unit Unit::operator/(const Unit &o) const { return *this * o.pow(-1); }

Unit Unit::pow(int n) const {
  Unit r;
  if (n == 0) return r;
  for (auto &[name, exp] : dims) r.dims[name] = exp * n;
  return r;
}

std::string Unit::str() const {
  std::string num, den;
  for (auto &[name, exp] : dims) {
    int e = exp > 0 ? exp : -exp;
    std::string factor = name + (e != 1 ? "^" + std::to_string(e) : "");
    if (exp > 0)
      num += (num.empty() ? "" : "*") + factor;
    else
      den += "/" + factor;
  }
  if (num.empty() && !den.empty()) num = "1";
  return num + den;
}

std::string Type::str() const {
  std::string s = kindName(kind);
  if (!unit.dimensionless()) s += "<" + unit.str() + ">";
  return array ? "[" + s + "]" : s;
}

std::vector<ExprPtr> takeExprs(std::vector<Expr *> *v) {
  std::vector<ExprPtr> out;
  if (v) {
    for (Expr *e : *v) out.emplace_back(e);
    delete v;
  }
  return out;
}

const char *binOpName(BinOp op) {
  switch (op) {
    case BinOp::Add: return "+";
    case BinOp::Sub: return "-";
    case BinOp::Mul: return "*";
    case BinOp::Div: return "/";
    case BinOp::Mod: return "%";
    case BinOp::Lt: return "<";
    case BinOp::Le: return "<=";
    case BinOp::Gt: return ">";
    case BinOp::Ge: return ">=";
    case BinOp::Eq: return "==";
    case BinOp::Ne: return "!=";
    case BinOp::And: return "&&";
    case BinOp::Or: return "||";
  }
  return "?";
}

void Diagnostics::error(Loc loc, const std::string &msg) {
  ++errors_;
  std::cerr << file_ << ":" << loc.line << ":" << loc.col << ": error: " << msg << "\n";
}

Diagnostics &diag() {
  static Diagnostics d;
  return d;
}

// ------------------------------------------------------------------ AST dump

namespace {

void indent(int n) { std::cout << std::string(n * 2, ' '); }

void dump(const Expr *e, int d);
void dump(const Stmt *s, int d);

void dump(const Expr *e, int d) {
  indent(d);
  if (auto *n = dynamic_cast<const IntLit *>(e)) {
    std::cout << "Int " << n->value << (n->unit.dimensionless() ? "" : " <" + n->unit.str() + ">") << "\n";
  } else if (auto *n = dynamic_cast<const FloatLit *>(e)) {
    std::cout << "Float " << n->value << (n->unit.dimensionless() ? "" : " <" + n->unit.str() + ">") << "\n";
  } else if (auto *n = dynamic_cast<const BoolLit *>(e)) {
    std::cout << "Bool " << (n->value ? "true" : "false") << "\n";
  } else if (auto *n = dynamic_cast<const StringLit *>(e)) {
    std::cout << "String \"" << n->value << "\"\n";
  } else if (auto *n = dynamic_cast<const VarRef *>(e)) {
    std::cout << "Var " << n->name << "\n";
  } else if (auto *n = dynamic_cast<const Unary *>(e)) {
    std::cout << "Unary " << (n->op == UnOp::Neg ? "-" : "!") << "\n";
    dump(n->operand.get(), d + 1);
  } else if (auto *n = dynamic_cast<const Binary *>(e)) {
    std::cout << "Binary " << binOpName(n->op) << "\n";
    dump(n->lhs.get(), d + 1);
    dump(n->rhs.get(), d + 1);
  } else if (auto *n = dynamic_cast<const Call *>(e)) {
    std::cout << "Call " << n->callee << "\n";
    for (auto &a : n->args) dump(a.get(), d + 1);
  } else if (auto *n = dynamic_cast<const Cast *>(e)) {
    std::cout << "Cast " << n->target.str() << "\n";
    dump(n->operand.get(), d + 1);
  } else if (auto *n = dynamic_cast<const ArrayLit *>(e)) {
    std::cout << "Array\n";
    for (auto &el : n->elements) dump(el.get(), d + 1);
  } else if (auto *n = dynamic_cast<const ArrayRepeat *>(e)) {
    std::cout << "ArrayRepeat\n";
    dump(n->value.get(), d + 1);
    dump(n->count.get(), d + 1);
  } else if (auto *n = dynamic_cast<const Index *>(e)) {
    std::cout << "Index\n";
    dump(n->base.get(), d + 1);
    dump(n->index.get(), d + 1);
  }
}

void dump(const Stmt *s, int d) {
  if (!s) return;
  indent(d);
  if (auto *n = dynamic_cast<const Block *>(s)) {
    std::cout << "Block\n";
    for (auto &st : n->stmts) dump(st.get(), d + 1);
  } else if (auto *n = dynamic_cast<const LetStmt *>(s)) {
    std::cout << (n->isMutable ? "Var " : "Let ") << n->name;
    if (n->declared.known()) std::cout << ": " << n->declared.str();
    std::cout << "\n";
    dump(n->init.get(), d + 1);
  } else if (auto *n = dynamic_cast<const AssignStmt *>(s)) {
    std::cout << "Assign" << (n->compound ? std::string(" ") + binOpName(n->op) + "=" : "") << "\n";
    dump(n->target.get(), d + 1);
    dump(n->value.get(), d + 1);
  } else if (auto *n = dynamic_cast<const ExprStmt *>(s)) {
    std::cout << "ExprStmt\n";
    dump(n->expr.get(), d + 1);
  } else if (auto *n = dynamic_cast<const ReturnStmt *>(s)) {
    std::cout << "Return\n";
    if (n->value) dump(n->value.get(), d + 1);
  } else if (auto *n = dynamic_cast<const IfStmt *>(s)) {
    std::cout << "If\n";
    dump(n->cond.get(), d + 1);
    dump(n->thenBranch.get(), d + 1);
    if (n->elseBranch) {
      indent(d);
      std::cout << "Else\n";
      dump(n->elseBranch.get(), d + 1);
    }
  } else if (auto *n = dynamic_cast<const WhileStmt *>(s)) {
    std::cout << "While\n";
    dump(n->cond.get(), d + 1);
    dump(n->body.get(), d + 1);
  } else if (auto *n = dynamic_cast<const ForStmt *>(s)) {
    std::cout << "For " << n->var << "\n";
    dump(n->start.get(), d + 1);
    dump(n->end.get(), d + 1);
    dump(n->body.get(), d + 1);
  } else if (auto *n = dynamic_cast<const ForEachStmt *>(s)) {
    std::cout << "ForEach " << n->var << "\n";
    dump(n->iterable.get(), d + 1);
    dump(n->body.get(), d + 1);
  } else if (dynamic_cast<const BreakStmt *>(s)) {
    std::cout << "Break\n";
  } else if (dynamic_cast<const ContinueStmt *>(s)) {
    std::cout << "Continue\n";
  }
}

}  // namespace

void dumpAst(const Program &prog) {
  for (auto &u : prog.units)
    std::cout << "Unit " << u.name << (u.alias ? " = " + u.alias->str() : "") << "\n";
  for (auto &f : prog.functions) {
    std::cout << "Function " << f->name << "(";
    for (size_t i = 0; i < f->params.size(); ++i) {
      if (i) std::cout << ", ";
      std::cout << (f->params[i].isMutable ? "var " : "") << f->params[i].name << ": " << f->params[i].type.str();
    }
    std::cout << ") -> " << f->returnType.str() << "\n";
    dump(f->body.get(), 1);
  }
}

}  // namespace nova
