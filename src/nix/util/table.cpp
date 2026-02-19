#include "nix/util/table.h"

#include <algorithm>
#include <cassert>
#include <iostream>
#include <vector>

#include "nix/util/terminal.h"

namespace nix {

void print_table(std::ostream& out, table_t& table, unsigned int width) {
  auto nr_columns = table.size() > 0 ? table.front().size() : 0;

  std::vector<size_t> widths;
  widths.resize(nr_columns);

  for (auto& i : table) {
    assert(i.size() == nr_columns);
    size_t column = 0;
    for (auto j = i.begin(); j != i.end(); ++j, ++column) {
      // TODO: take ANSI escapes into account when calculating width.
      widths[column] = std::max(widths[column], j->content.size());
}
  }

  for (auto& i : table) {
    size_t column = 0;
    std::string line;
    for (auto j = i.begin(); j != i.end(); ++j, ++column) {
      std::string s = j->content;
      replace(s.begin(), s.end(), '\n', ' ');

      auto padding = std::string(widths[column] - s.size(), ' ');
      if (j->alignment == table_cell_t::right) {
        line += padding;
        line += s;
      } else {
        line += s;
        if (column + 1 < nr_columns) {
          line += padding;
}
      }

      if (column + 1 < nr_columns) {
        line += "  ";
}
    }
    out << filter_ansi_escapes(line, false, width);
    out << std::endl;
  }
}

} // namespace nix
