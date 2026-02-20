#pragma once
/**
 * @file
 * @brief common_t printing functions for the Nix language
 *
 * While most types come with their own methods for printing, they share some
 * functions that are placed here.
 */

#include <iostream>

#include "nix/expr/print-options.h"
#include "nix/util/fmt.h"

namespace nix {

class eval_state_t;
struct value_t;

/**
 * Print a string as a Nix string literal.
 *
 * Quotes and fairly minimal escaping are added.
 *
 * @param o The output stream to print to
 * @param s The logical string
 */
std::ostream& print_literal_string(std::ostream& o, std::string_view s);

inline std::ostream& print_literal_string(std::ostream& o, const char* s) {
  return print_literal_string(o, std::string_view(s));
}

inline std::ostream& print_literal_string(std::ostream& o, const std::string& s) {
  return print_literal_string(o, std::string_view(s));
}

/** Print `true` or `false`. */
std::ostream& print_literal_bool(std::ostream& o, bool b);

/**
 * Print a string as an attribute name in the Nix expression language syntax.
 *
 * Prints a quoted string if necessary.
 */
std::ostream& print_attribute_name(std::ostream& o, std::string_view s);

/**
 * Returns `true' is a string is a reserved keyword which requires quotation
 * when printing attribute set field names.
 */
bool is_reserved_keyword(const std::string_view str);

/**
 * Print a string as an identifier in the Nix expression language syntax.
 *
 * FIXME: "identifier" is ambiguous. Identifiers do not have a single
 *        textual representation. They can be used in variable references,
 *        let bindings, left-hand sides or attribute names in a select
 *        expression, or something else entirely, like JSON. use one of the
 *        `print*` functions instead.
 */
std::ostream& print_identifier(std::ostream& o, std::string_view s);

void print_value(eval_state_t& state, std::ostream& str, value_t& v,
                 PrintOptions options = PrintOptions{});

/**
 * A partially-applied form of `print_value` which can be formatted using `<<`
 * without allocating an intermediate string.
 */
class ValuePrinter {
  friend std::ostream& operator<<(std::ostream& output, const ValuePrinter& printer);

private:
  eval_state_t& state;
  value_t& value;
  PrintOptions options;

public:
  ValuePrinter(eval_state_t& state, value_t& value, PrintOptions options = PrintOptions{})
      : state(state), value(value), options(options) {}
};

std::ostream& operator<<(std::ostream& output, const ValuePrinter& printer);

/**
 * `ValuePrinter` does its own ANSI formatting, so we don't color it
 * magenta.
 */
template <>
hint_fmt_t& hint_fmt_t::operator%(const ValuePrinter& value);

} // namespace nix
