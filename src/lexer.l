%{
// NovaLang lexer.
#include <cstdlib>
#include <string>
#include "ast.h"
#include "diagnostics.h"
#include "parser.hpp"

static int yycolumn = 1;

#define YY_USER_ACTION                                   \
  yylloc.first_line = yylloc.last_line = yylineno;       \
  yylloc.first_column = yycolumn;                        \
  yycolumn += yyleng;                                    \
  yylloc.last_column = yycolumn - 1;

static std::string unescape(const char *text, int len) {
  std::string out;
  for (int i = 1; i < len - 1; ++i) {
    char c = text[i];
    if (c == '\\' && i + 1 < len - 1) {
      char n = text[++i];
      switch (n) {
        case 'n': out += '\n'; break;
        case 't': out += '\t'; break;
        case '\\': out += '\\'; break;
        case '"': out += '"'; break;
        case '0': out += '\0'; break;
        default:
          nova::diag().error({yylineno, yycolumn - len + i - 1},
                             std::string("unknown escape sequence '\\") + n + "'");
      }
    } else {
      out += c;
    }
  }
  return out;
}

// Parses a unit annotation such as "<kg*m/s^2>" into its (unresolved) factors.
// Every factor after a '/' is divided, so m/s/s == m/s^2.
static nova::Unit *parseUnit(const char *text, int len) {
  auto *u = new nova::Unit();
  std::string s(text + 1, len - 2);
  size_t i = 0;
  int sign = 1;
  while (i < s.size()) {
    size_t j = i;
    while (j < s.size() && s[j] != '*' && s[j] != '/' && s[j] != '^') ++j;
    std::string name = s.substr(i, j - i);
    int exp = 1;
    if (j < s.size() && s[j] == '^') {
      size_t k = ++j;
      while (k < s.size() && s[k] != '*' && s[k] != '/') ++k;
      exp = std::atoi(s.substr(j, k - j).c_str());
      j = k;
    }
    if (name != "1") {
      int &d = u->dims[name];
      d += sign * exp;
      if (d == 0) u->dims.erase(name);
    }
    if (j < s.size()) sign = s[j] == '/' ? -1 : 1;
    i = j + 1;
  }
  return u;
}
%}

%option noyywrap nounput noinput yylineno

%x COMMENT

DIGIT   [0-9]
IDENT   [a-zA-Z_][a-zA-Z0-9_]*
UFACTOR ({IDENT}|1)(\^-?{DIGIT}+)?

%%

"//"[^\n]*              { /* line comment */ }
"/*"                    { BEGIN(COMMENT); }
<COMMENT>"*/"           { BEGIN(INITIAL); }
<COMMENT>\n             { yycolumn = 1; }
<COMMENT>.              { }
<COMMENT><<EOF>>        { nova::diag().error({yylineno, yycolumn}, "unterminated block comment"); BEGIN(INITIAL); return 0; }

[ \t\r]+                { }
\n                      { yycolumn = 1; }

"fn"                    { return FN; }
"let"                   { return LET; }
"var"                   { return VAR; }
"if"                    { return IF; }
"else"                  { return ELSE; }
"while"                 { return WHILE; }
"for"                   { return FOR; }
"in"                    { return IN; }
"return"                { return RETURN; }
"break"                 { return BREAK; }
"continue"              { return CONTINUE; }
"as"                    { return AS; }
"unit"                  { return UNIT_KW; }
"true"                  { yylval.boolVal = true; return BOOL_LIT; }
"false"                 { yylval.boolVal = false; return BOOL_LIT; }
"int"                   { yylval.kind = nova::Kind::Int; return TYPE; }
"float"                 { yylval.kind = nova::Kind::Float; return TYPE; }
"bool"                  { yylval.kind = nova::Kind::Bool; return TYPE; }
"string"                { yylval.kind = nova::Kind::String; return TYPE; }
"void"                  { yylval.kind = nova::Kind::Void; return TYPE; }

 /* A unit annotation is written with no spaces inside, e.g. 9.8<m/s^2> or float<m>. */
"<"{UFACTOR}([*/]{UFACTOR})*">" { yylval.unit = parseUnit(yytext, yyleng); return UNIT_ANNOT; }

{DIGIT}+"."{DIGIT}+([eE][+-]?{DIGIT}+)? { yylval.floatVal = std::stod(yytext); return FLOAT_LIT; }
{DIGIT}+                {
                          try {
                            yylval.intVal = std::stoll(yytext);
                          } catch (...) {
                            nova::diag().error({yylineno, yycolumn - (int)yyleng}, "integer literal out of range");
                            yylval.intVal = 0;
                          }
                          return INT_LIT;
                        }
\"(\\.|[^\\"\n])*\"     { yylval.str = new std::string(unescape(yytext, yyleng)); return STRING_LIT; }
\"(\\.|[^\\"\n])*       { nova::diag().error({yylineno, yycolumn - (int)yyleng}, "unterminated string literal");
                          yylval.str = new std::string(); return STRING_LIT; }
{IDENT}                 { yylval.str = new std::string(yytext); return IDENT; }

"->"                    { return ARROW; }
"|>"                    { return PIPE; }
".."                    { return DOTDOT; }
"=="                    { return EQ; }
"!="                    { return NE; }
"<="                    { return LE; }
">="                    { return GE; }
"&&"                    { return AND; }
"||"                    { return OR; }
"+="                    { yylval.binop = nova::BinOp::Add; return COMPOUND; }
"-="                    { yylval.binop = nova::BinOp::Sub; return COMPOUND; }
"*="                    { yylval.binop = nova::BinOp::Mul; return COMPOUND; }
"/="                    { yylval.binop = nova::BinOp::Div; return COMPOUND; }
"%="                    { yylval.binop = nova::BinOp::Mod; return COMPOUND; }
[-+*/%<>=!(){}\[\],;:]  { return yytext[0]; }

.                       { nova::diag().error({yylineno, yycolumn - 1},
                                             std::string("unexpected character '") + yytext + "'"); }

%%

void novaLexerReset(FILE *in) {
  yyin = in;
  yylineno = 1;
  yycolumn = 1;
  YY_FLUSH_BUFFER;
  BEGIN(INITIAL);
}
