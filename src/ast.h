// NovaLang abstract syntax tree and type representation.
#pragma once

#include <map>
#include <memory>
#include <string>
#include <vector>

namespace nova {

// ---------------------------------------------------------------------- units

// A unit of measure as a product of named base units raised to integer powers,
// e.g. kg*m/s^2 is {kg: 1, m: 1, s: -2}. An empty map is dimensionless.
struct Unit {
  std::map<std::string, int> dims;

  bool dimensionless() const { return dims.empty(); }
  Unit operator*(const Unit &o) const;
  Unit operator/(const Unit &o) const;
  Unit pow(int n) const;
  bool operator==(const Unit &o) const { return dims == o.dims; }
  bool operator!=(const Unit &o) const { return dims != o.dims; }
  std::string str() const;  // "kg*m/s^2", "1/s", "" when dimensionless
};

// ---------------------------------------------------------------------- types

enum class Kind { Unknown, Int, Float, Bool, String, Void, Struct };

struct Type {
  Kind kind = Kind::Unknown;
  bool array = false;      // [kind<unit>]
  Unit unit;               // only meaningful for int and float (or arrays of them)
  std::string structName;  // when kind == Struct

  Type() = default;
  Type(Kind k) : kind(k) {}  // NOLINT: implicit so Kind::Int converts naturally

  bool known() const { return kind != Kind::Unknown; }
  bool isNumeric() const { return !array && (kind == Kind::Int || kind == Kind::Float); }
  // Anything that can be stored in a variable, array element or struct field.
  bool isValue() const { return !array && kind != Kind::Void && kind != Kind::Unknown; }
  // Same representation, ignoring units.
  bool sameShape(const Type &o) const { return kind == o.kind && array == o.array && structName == o.structName; }
  bool is(Kind k) const { return !array && kind == k && unit.dimensionless(); }
  Type element() const {
    Type t = *this;
    t.array = false;
    return t;
  }
  Type arrayOf() const {
    Type t = *this;
    t.array = true;
    return t;
  }
  bool operator==(const Type &o) const { return sameShape(o) && unit == o.unit; }
  bool operator!=(const Type &o) const { return !(*this == o); }
  std::string str() const;  // "int", "float<m/s>", "[int]", "Vec2"
};

const char *kindName(Kind k);

struct Loc {
  int line = 0;
  int col = 0;
};

// ---------------------------------------------------------------- expressions

struct Expr {
  Loc loc;
  Type type;  // filled in by semantic analysis
  virtual ~Expr() = default;
};
using ExprPtr = std::unique_ptr<Expr>;

struct IntLit : Expr {
  long long value;
  Unit unit;  // as written; resolved by sema
  IntLit(long long v, Unit u = {}) : value(v), unit(std::move(u)) {}
};

struct FloatLit : Expr {
  double value;
  Unit unit;
  FloatLit(double v, Unit u = {}) : value(v), unit(std::move(u)) {}
};

struct BoolLit : Expr {
  bool value;
  explicit BoolLit(bool v) : value(v) {}
};

struct StringLit : Expr {
  std::string value;
  explicit StringLit(std::string v) : value(std::move(v)) {}
};

struct VarRef : Expr {
  std::string name;
  explicit VarRef(std::string n) : name(std::move(n)) {}
};

enum class UnOp { Neg, Not };

struct Unary : Expr {
  UnOp op;
  ExprPtr operand;
  Unary(UnOp o, Expr *e) : op(o), operand(e) {}
};

enum class BinOp { Add, Sub, Mul, Div, Mod, Lt, Le, Gt, Ge, Eq, Ne, And, Or };

const char *binOpName(BinOp op);

struct Binary : Expr {
  BinOp op;
  ExprPtr lhs, rhs;
  Binary(BinOp o, Expr *l, Expr *r) : op(o), lhs(l), rhs(r) {}
};

std::vector<ExprPtr> takeExprs(std::vector<Expr *> *v);

struct Call : Expr {
  std::string callee;
  std::vector<ExprPtr> args;
  Call(std::string c, std::vector<Expr *> *a) : callee(std::move(c)), args(takeExprs(a)) {}
};

struct Cast : Expr {
  ExprPtr operand;
  Type target;
  Cast(Expr *e, Type t) : operand(e), target(std::move(t)) {}
};

// [a, b, c]
struct ArrayLit : Expr {
  std::vector<ExprPtr> elements;
  explicit ArrayLit(std::vector<Expr *> *e) : elements(takeExprs(e)) {}
};

// [value; count]
struct ArrayRepeat : Expr {
  ExprPtr value, count;
  ArrayRepeat(Expr *v, Expr *c) : value(v), count(c) {}
};

// base[index]
struct Index : Expr {
  ExprPtr base, index;
  bool checked = true;  // cleared by sema when the index is provably in bounds
  Index(Expr *b, Expr *i) : base(b), index(i) {}
};

// base.field
struct FieldAccess : Expr {
  ExprPtr base;
  std::string field;
  int fieldIndex = -1;  // resolved by sema
  FieldAccess(Expr *b, std::string f) : base(b), field(std::move(f)) {}
};

struct FieldInit {
  std::string name;
  ExprPtr value;
  Loc loc;
};

// Name(field: value, ...)
struct StructLit : Expr {
  std::string name;
  std::vector<FieldInit> fields;
  explicit StructLit(std::string n) : name(std::move(n)) {}
};

// ----------------------------------------------------------------- statements

struct Stmt {
  Loc loc;
  virtual ~Stmt() = default;
};
using StmtPtr = std::unique_ptr<Stmt>;

struct Block : Stmt {
  std::vector<StmtPtr> stmts;
  explicit Block(std::vector<Stmt *> *s) {
    if (s) {
      for (Stmt *st : *s) stmts.emplace_back(st);
      delete s;
    }
  }
};

struct LetStmt : Stmt {
  std::string name;
  bool isMutable;
  Type declared;  // Unknown when the type is inferred
  ExprPtr init;
  LetStmt(std::string n, bool m, Type d, Expr *i)
      : name(std::move(n)), isMutable(m), declared(std::move(d)), init(i) {}
};

// target = value, or target op= value for compound assignment.
struct AssignStmt : Stmt {
  ExprPtr target;  // VarRef or Index
  bool compound;
  BinOp op;  // valid when compound
  ExprPtr value;
  AssignStmt(Expr *t, bool c, BinOp o, Expr *v) : target(t), compound(c), op(o), value(v) {}
};

struct ExprStmt : Stmt {
  ExprPtr expr;
  explicit ExprStmt(Expr *e) : expr(e) {}
};

struct ReturnStmt : Stmt {
  ExprPtr value;  // may be null
  explicit ReturnStmt(Expr *v) : value(v) {}
};

struct IfStmt : Stmt {
  ExprPtr cond;
  StmtPtr thenBranch;  // Block
  StmtPtr elseBranch;  // Block, IfStmt, or null
  IfStmt(Expr *c, Stmt *t, Stmt *e) : cond(c), thenBranch(t), elseBranch(e) {}
};

struct WhileStmt : Stmt {
  ExprPtr cond;
  StmtPtr body;
  WhileStmt(Expr *c, Stmt *b) : cond(c), body(b) {}
};

// for i in start..end { ... }   (end is exclusive)
struct ForStmt : Stmt {
  std::string var;
  ExprPtr start, end;
  StmtPtr body;
  ForStmt(std::string v, Expr *s, Expr *e, Stmt *b)
      : var(std::move(v)), start(s), end(e), body(b) {}
};

// for x in array { ... }
struct ForEachStmt : Stmt {
  std::string var;
  ExprPtr iterable;
  StmtPtr body;
  ForEachStmt(std::string v, Expr *it, Stmt *b) : var(std::move(v)), iterable(it), body(b) {}
};

struct BreakStmt : Stmt {};
struct ContinueStmt : Stmt {};

// ------------------------------------------------------------------ top level

struct Param {
  std::string name;
  Type type;
  bool isMutable;
  Loc loc;
};

struct Function {
  Loc loc;
  std::string name;
  std::vector<Param> params;
  Type returnType;
  std::unique_ptr<Block> body;
};

struct FieldDecl {
  std::string name;
  Type type;
  Loc loc;
};

// struct Name { field: type, ... }
struct StructDecl {
  Loc loc;
  std::string name;
  std::vector<FieldDecl> fields;
};

// unit m;            (base unit, alias is null)
// unit N = <kg*m/s^2>;
struct UnitDecl {
  Loc loc;
  std::string name;
  std::unique_ptr<Unit> alias;
};

struct Program {
  std::vector<UnitDecl> units;
  std::vector<StructDecl> structs;
  std::vector<std::unique_ptr<Function>> functions;
};

void dumpAst(const Program &prog);

}  // namespace nova
