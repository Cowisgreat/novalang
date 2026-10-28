// Semantic analysis: name resolution, type checking and type inference.
#pragma once

#include "ast.h"

namespace nova {

// Annotates every expression with its type. Errors go to diag().
void analyze(Program &prog);

}  // namespace nova
