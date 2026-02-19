#pragma once

#include <limits>

#include "nix/util/types.h"

namespace nix {

struct table_cell_t {
  std::string content;

  enum alignment_t { left, right } alignment = left;

  table_cell_t(std::string content, alignment_t alignment = left)
      : content(std::move(content)), alignment(alignment) {}
};

using table_row_t = std::vector<table_cell_t>;
using table_t = std::vector<table_row_t>;

void print_table(std::ostream& out, table_t& table,
                unsigned int width = std::numeric_limits<unsigned int>::max());

} // namespace nix
