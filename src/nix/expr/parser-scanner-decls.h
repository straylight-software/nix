#pragma once

#ifndef BISON_HEADER
#  include "parser-tab.h"
using YYSTYPE = nix::parser::bison_parser_t::value_type;
using YYLTYPE = nix::parser::bison_parser_t::location_type;
#  include "lexer-tab.h" // IWYU pragma: export
#endif

namespace nix {

class Parser : public parser::bison_parser_t {
  using bison_parser_t::bison_parser_t;
};

} // namespace nix
