%{
// NovaLang grammar.
#include <cstdio>
#include <string>
#include <vector>
#include "ast.h"
#include "diagnostics.h"

using namespace nova;

int yylex();
void yyerror(const char *msg);

Program *novaParsedProgram = nullptr;
%}

%code {
static Loc L(const YYLTYPE &l) { return {l.first_line, l.first_column}; }

template <typename T>
static T *at(T *node, const YYLTYPE &l) {
  node->loc = L(l);
  return node;
}

static Type take(Type *t) {
  Type v = std::move(*t);
  delete t;
  return v;
}

// lhs |> f(args)  ==>  f(lhs, args)
static Call *pipe(Expr *lhs, std::string *callee, std::vector<Expr *> *args) {
  if (!args) args = new std::vector<Expr *>();
  args->insert(args->begin(), lhs);
  auto *c = new Call(*callee, args);
  delete callee;
  return c;
}
}

%locations
%define parse.error verbose

%code requires {
#include <string>
#include <vector>
#include "ast.h"
}

%union {
  long long intVal;
  double floatVal;
  bool boolVal;
  std::string *str;
  nova::Kind kind;
  nova::BinOp binop;
  nova::Unit *unit;
  nova::Type *type;
  nova::Expr *expr;
  nova::Stmt *stmt;
  nova::Block *block;
  nova::Function *func;
  std::vector<nova::Expr *> *exprs;
  std::vector<nova::Stmt *> *stmts;
  std::vector<nova::Param> *params;
}

%token FN LET VAR IF ELSE WHILE FOR IN RETURN BREAK CONTINUE AS UNIT_KW
%token ARROW PIPE DOTDOT EQ NE LE GE AND OR
%token <intVal> INT_LIT
%token <floatVal> FLOAT_LIT
%token <boolVal> BOOL_LIT
%token <str> STRING_LIT IDENT
%token <kind> TYPE
%token <unit> UNIT_ANNOT
%token <binop> COMPOUND

%type <expr> expr
%type <stmt> stmt else_part
%type <block> block
%type <func> function
%type <exprs> args arg_list
%type <stmts> stmts
%type <params> params param_list
%type <type> type_spec ret_type opt_type
%type <boolVal> opt_var

%destructor { delete $$; } <str> <unit> <type>

%precedence PIPE
%left OR
%left AND
%left EQ NE
%left '<' '>' LE GE
%left '+' '-'
%left '*' '/' '%'
%precedence AS
%precedence UNARY
%precedence '['

%%

program
  : %empty                { novaParsedProgram = new Program(); }
  | program function      { novaParsedProgram->functions.emplace_back($2); }
  | program unit_decl
  ;

unit_decl
  : UNIT_KW IDENT ';'     { novaParsedProgram->units.push_back({L(@2), *$2, nullptr}); delete $2; }
  | UNIT_KW IDENT '=' UNIT_ANNOT ';' {
      novaParsedProgram->units.push_back({L(@2), *$2, std::unique_ptr<Unit>($4)});
      delete $2;
    }
  ;

function
  : FN IDENT '(' params ')' ret_type block {
      $$ = new Function();
      $$->loc = L(@1);
      $$->name = *$2; delete $2;
      $$->params = std::move(*$4); delete $4;
      $$->returnType = take($6);
      $$->body.reset($7);
    }
  ;

type_spec
  : TYPE                  { $$ = new Type($1); }
  | TYPE UNIT_ANNOT       { $$ = new Type($1); $$->unit = *$2; delete $2; }
  | '[' type_spec ']'     {
      if ($2->array) diag().error(L(@1), "nested arrays are not supported");
      $$ = $2; $$->array = true;
    }
  ;

ret_type
  : %empty                { $$ = new Type(Kind::Void); }
  | ARROW type_spec       { $$ = $2; }
  ;

params
  : %empty                { $$ = new std::vector<Param>(); }
  | param_list            { $$ = $1; }
  ;

opt_var
  : %empty                { $$ = false; }
  | VAR                   { $$ = true; }
  ;

param_list
  : opt_var IDENT ':' type_spec {
      $$ = new std::vector<Param>();
      $$->push_back({*$2, take($4), $1, L(@2)}); delete $2;
    }
  | param_list ',' opt_var IDENT ':' type_spec {
      $$ = $1;
      $$->push_back({*$4, take($6), $3, L(@4)}); delete $4;
    }
  ;

block
  : '{' stmts '}'         { $$ = at(new Block($2), @1); }
  ;

stmts
  : %empty                { $$ = new std::vector<Stmt *>(); }
  | stmts stmt            { $$ = $1; if ($2) $$->push_back($2); }
  ;

opt_type
  : %empty                { $$ = new Type(); }
  | ':' type_spec         { $$ = $2; }
  ;

stmt
  : LET IDENT opt_type '=' expr ';'   { $$ = at(new LetStmt(*$2, false, take($3), $5), @1); delete $2; }
  | VAR IDENT opt_type '=' expr ';'   { $$ = at(new LetStmt(*$2, true, take($3), $5), @1); delete $2; }
  | expr '=' expr ';'                 { $$ = at(new AssignStmt($1, false, BinOp::Add, $3), @1); }
  | expr COMPOUND expr ';'            { $$ = at(new AssignStmt($1, true, $2, $3), @1); }
  | expr ';'                          { $$ = at(new ExprStmt($1), @1); }
  | RETURN expr ';'                   { $$ = at(new ReturnStmt($2), @1); }
  | RETURN ';'                        { $$ = at(new ReturnStmt(nullptr), @1); }
  | IF expr block else_part           { $$ = at(new IfStmt($2, $3, $4), @1); }
  | WHILE expr block                  { $$ = at(new WhileStmt($2, $3), @1); }
  | FOR IDENT IN expr DOTDOT expr block { $$ = at(new ForStmt(*$2, $4, $6, $7), @1); delete $2; }
  | FOR IDENT IN expr block           { $$ = at(new ForEachStmt(*$2, $4, $5), @1); delete $2; }
  | BREAK ';'                         { $$ = at(new BreakStmt(), @1); }
  | CONTINUE ';'                      { $$ = at(new ContinueStmt(), @1); }
  | block                             { $$ = $1; }
  | error ';'                         { $$ = nullptr; yyerrok; }
  ;

else_part
  : %empty                { $$ = nullptr; }
  | ELSE block            { $$ = $2; }
  | ELSE IF expr block else_part { $$ = at(new IfStmt($3, $4, $5), @2); }
  ;

expr
  : INT_LIT               { $$ = at(new IntLit($1), @1); }
  | INT_LIT UNIT_ANNOT    { $$ = at(new IntLit($1, *$2), @1); delete $2; }
  | FLOAT_LIT             { $$ = at(new FloatLit($1), @1); }
  | FLOAT_LIT UNIT_ANNOT  { $$ = at(new FloatLit($1, *$2), @1); delete $2; }
  | BOOL_LIT              { $$ = at(new BoolLit($1), @1); }
  | STRING_LIT            { $$ = at(new StringLit(*$1), @1); delete $1; }
  | IDENT                 { $$ = at(new VarRef(*$1), @1); delete $1; }
  | IDENT '(' args ')'    { $$ = at(new Call(*$1, $3), @1); delete $1; }
  | '[' arg_list ']'      { $$ = at(new ArrayLit($2), @1); }
  | '[' ']'               { diag().error(L(@1), "empty array literal: use [value; 0] to give the element type");
                            $$ = at(new ArrayLit(nullptr), @1); }
  | '[' expr ';' expr ']' { $$ = at(new ArrayRepeat($2, $4), @1); }
  | expr '[' expr ']'     { $$ = at(new Index($1, $3), @2); }
  | '(' expr ')'          { $$ = $2; }
  | '-' expr %prec UNARY  { $$ = at(new Unary(UnOp::Neg, $2), @1); }
  | '!' expr %prec UNARY  { $$ = at(new Unary(UnOp::Not, $2), @1); }
  | expr AS type_spec     { $$ = at(new Cast($1, take($3)), @2); }
  | expr PIPE IDENT       { $$ = at(pipe($1, $3, nullptr), @2); }
  | expr PIPE IDENT '(' args ')' { $$ = at(pipe($1, $3, $5), @2); }
  | expr '+' expr         { $$ = at(new Binary(BinOp::Add, $1, $3), @2); }
  | expr '-' expr         { $$ = at(new Binary(BinOp::Sub, $1, $3), @2); }
  | expr '*' expr         { $$ = at(new Binary(BinOp::Mul, $1, $3), @2); }
  | expr '/' expr         { $$ = at(new Binary(BinOp::Div, $1, $3), @2); }
  | expr '%' expr         { $$ = at(new Binary(BinOp::Mod, $1, $3), @2); }
  | expr '<' expr         { $$ = at(new Binary(BinOp::Lt, $1, $3), @2); }
  | expr '>' expr         { $$ = at(new Binary(BinOp::Gt, $1, $3), @2); }
  | expr LE expr          { $$ = at(new Binary(BinOp::Le, $1, $3), @2); }
  | expr GE expr          { $$ = at(new Binary(BinOp::Ge, $1, $3), @2); }
  | expr EQ expr          { $$ = at(new Binary(BinOp::Eq, $1, $3), @2); }
  | expr NE expr          { $$ = at(new Binary(BinOp::Ne, $1, $3), @2); }
  | expr AND expr         { $$ = at(new Binary(BinOp::And, $1, $3), @2); }
  | expr OR expr          { $$ = at(new Binary(BinOp::Or, $1, $3), @2); }
  ;

args
  : %empty                { $$ = nullptr; }
  | arg_list              { $$ = $1; }
  ;

arg_list
  : expr                  { $$ = new std::vector<Expr *>(); $$->push_back($1); }
  | arg_list ',' expr     { $$ = $1; $$->push_back($3); }
  ;

%%

void yyerror(const char *msg) {
  diag().error({yylloc.first_line, yylloc.first_column}, msg);
}
