#include "nix/util/position.h"

namespace nix {

pos_t::operator std::shared_ptr<const pos_t>() const {
  return std::make_shared<const pos_t>(*this);
}

std::optional<lines_of_code_t> pos_t::get_code_lines() const {
  if (line == 0)
    return std::nullopt;

  if (auto source = get_source()) {
    lines_iterator_t lines(*source), end;
    lines_of_code_t loc;

    if (line > 1)
      std::advance(lines, line - 2);
    if (lines != end && line > 1)
      loc.prev_line_of_code = *lines++;
    if (lines != end)
      loc.err_line_of_code = *lines++;
    if (lines != end)
      loc.next_line_of_code = *lines++;

    return loc;
  }

  return std::nullopt;
}

std::optional<std::string> pos_t::get_source() const {
  return std::visit(
      overloaded{[](const std::monostate&) -> std::optional<std::string> { return std::nullopt; },
                 [](const pos_t::Stdin& s) -> std::optional<std::string> {
                   // Get rid of the null terminators added by the parser.
                   return std::string(s.source->c_str());
                 },
                 [](const pos_t::String& s) -> std::optional<std::string> {
                   // Get rid of the null terminators added by the parser.
                   return std::string(s.source->c_str());
                 },
                 [](const source_path_t& path) -> std::optional<std::string> {
                   try {
                     return path.read_file();
                   } catch (Error&) {
                     return std::nullopt;
                   }
                 }},
      origin);
}

std::optional<source_path_t> pos_t::get_source_path() const {
  if (auto* path = std::get_if<source_path_t>(&origin))
    return *path;
  return std::nullopt;
}

void pos_t::print(std::ostream& out, bool show_origin) const {
  if (show_origin) {
    std::visit(overloaded{[&](const std::monostate&) { out << "«none»"; },
                          [&](const pos_t::Stdin&) { out << "«stdin»"; },
                          [&](const pos_t::String& s) { out << "«string»"; },
                          [&](const source_path_t& path) { out << path; }},
               origin);
    out << ":";
  }
  out << line;
  if (column > 0)
    out << ":" << column;
}

std::ostream& operator<<(std::ostream& str, const pos_t& pos) {
  pos.print(str, true);
  return str;
}

void pos_t::lines_iterator_t::bump(bool at_first) {
  if (!at_first) {
    pastEnd = input.empty();
    if (!input.empty() && input[0] == '\r')
      input.remove_prefix(1);
    if (!input.empty() && input[0] == '\n')
      input.remove_prefix(1);
  }

  // nix line endings are not only \n as eg std::getline assumes, but also
  // \r\n **and \r alone**. not treating them all the same causes error
  // reports to not match with line numbers as the parser expects them.
  auto eol = input.find_first_of("\r\n");

  if (eol > input.size())
    eol = input.size();

  curLine = input.substr(0, eol);
  input.remove_prefix(eol);
}

std::optional<std::string> pos_t::get_snippet_up_to(const pos_t& end) const {
  assert(this->origin == end.origin);

  if (end.line < this->line)
    return std::nullopt;

  if (auto source = get_source()) {
    auto first_line = lines_iterator_t(*source);
    for (uint32_t i = 1; i < this->line; ++i) {
      ++first_line;
    }

    auto last_line = lines_iterator_t(*source);
    for (uint32_t i = 1; i < end.line; ++i) {
      ++last_line;
    }

    lines_iterator_t lines_end;

    std::string result;
    for (auto i = first_line; i != lines_end; ++i) {
      auto first_column = i == first_line ? (this->column ? this->column - 1 : 0) : 0;
      if (first_column > i->size())
        first_column = i->size();

      auto last_column =
          i == last_line ? (end.column ? end.column - 1 : 0) : std::numeric_limits<int>::max();
      if (last_column < first_column)
        last_column = first_column;
      if (last_column > i->size())
        last_column = i->size();

      result += i->substr(first_column, last_column - first_column);

      if (i == last_line) {
        break;
      } else {
        result += '\n';
      }
    }
    return result;
  }
  return std::nullopt;
}

} // namespace nix
