#pragma once

#include <cstddef>

#include "parser-scanner-decls.h"

namespace nix::lexer::internal {

void init_loc(Parser::location_type* loc);

void adjust_loc(yyscan_t yyscanner, Parser::location_type* loc, const char* s, size_t len);

} // namespace nix::lexer::internal
