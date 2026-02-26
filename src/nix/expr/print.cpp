#include "nix/expr/print.h"

#include <limits>
#include <sstream>

#include <boost/unordered/unordered_flat_set.hpp>

#include "nix/expr/eval.h"
#include "nix/store/store-api.h"
#include "nix/util/ansicolor.h"
#include "nix/util/english.h"
#include "nix/util/signals.h"
#include "nix/util/terminal.h"

namespace nix {

void print_elided(std::ostream& output, unsigned int value, const std::string_view single,
                  const std::string_view plural, bool ansi_colors) {
  if (ansi_colors)
    output << ANSI_FAINT;
  output << "«";
  pluralize(output, value, single, plural);
  output << " elided»";
  if (ansi_colors)
    output << ANSI_NORMAL;
}

std::ostream& print_literal_string(std::ostream& str, const std::string_view string,
                                   size_t max_length, bool ansi_colors) {
  size_t chars_printed = 0;
  if (ansi_colors)
    str << ANSI_MAGENTA;
  str << "\"";
  for (auto i = string.begin(); i != string.end(); ++i) {
    if (chars_printed >= max_length) {
      str << "\" ";
      print_elided(str, string.length() - chars_printed, "byte", "bytes", ansi_colors);
      return str;
    }

    if (*i == '\"' || *i == '\\')
      str << "\\" << *i;
    else if (*i == '\n')
      str << "\\n";
    else if (*i == '\r')
      str << "\\r";
    else if (*i == '\t')
      str << "\\t";
    else if (*i == '$' && *(i + 1) == '{')
      str << "\\" << *i;
    else
      str << *i;
    chars_printed++;
  }
  str << "\"";
  if (ansi_colors)
    str << ANSI_NORMAL;
  return str;
}

std::ostream& print_literal_string(std::ostream& str, const std::string_view string) {
  return print_literal_string(str, string, std::numeric_limits<size_t>::max(), false);
}

std::ostream& print_literal_bool(std::ostream& str, bool boolean) {
  str << (boolean ? "true" : "false");
  return str;
}

// Returns `true' is a string is a reserved keyword which requires quotation
// when printing attribute set field names.
//
// This list should generally be kept in sync with `./lexer.l'.
// You can test if a keyword needs to be added by running:
//   $ nix eval --expr '{ <KEYWORD> = 1; }'
// For example `or' doesn't need to be quoted.
bool is_reserved_keyword(const std::string_view str) {
  static const boost::unordered_flat_set<std::string_view> reserved_keywords = {
      "if", "then", "else", "assert", "with", "let", "in", "rec", "inherit"};
  return reserved_keywords.contains(str);
}

std::ostream& print_identifier(std::ostream& str, std::string_view s) {
  if (s.empty())
    str << "\"\"";
  else if (is_reserved_keyword(s))
    str << '"' << s << '"';
  else {
    char c = s[0];
    if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_')) {
      print_literal_string(str, s);
      return str;
    }
    for (auto c : s)
      if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
            c == '_' || c == '\'' || c == '-')) {
        print_literal_string(str, s);
        return str;
      }
    str << s;
  }
  return str;
}

static bool is_var_name(std::string_view s) {
  if (s.size() == 0)
    return false;
  if (is_reserved_keyword(s))
    return false;
  char c = s[0];
  if ((c >= '0' && c <= '9') || c == '-' || c == '\'')
    return false;
  for (auto& i : s)
    if (!((i >= 'a' && i <= 'z') || (i >= 'A' && i <= 'Z') || (i >= '0' && i <= '9') || i == '_' ||
          i == '-' || i == '\''))
      return false;
  return true;
}

std::ostream& print_attribute_name(std::ostream& str, std::string_view name) {
  if (is_var_name(name))
    str << name;
  else
    print_literal_string(str, name);
  return str;
}

bool is_important_attr_name(const std::string& attr_name) {
  return attr_name == "type" || attr_name == "_type";
}

typedef std::pair<std::string, value_t*> attr_pair_t;

struct important_first_attr_name_cmp_t {
  bool operator()(const attr_pair_t& lhs, const attr_pair_t& rhs) const {
    auto lhs_is_important = is_important_attr_name(lhs.first);
    auto rhs_is_important = is_important_attr_name(rhs.first);
    return std::forward_as_tuple(!lhs_is_important, lhs.first) <
           std::forward_as_tuple(!rhs_is_important, rhs.first);
  }
};

typedef std::set<const void*> values_seen_t;
typedef std::vector<std::pair<std::string, value_t*>> AttrVec;

struct printer_t {
  std::ostream& output;
  eval_state_t& state;
  PrintOptions options;
  std::optional<values_seen_t> seen;
  size_t totalAttrsPrinted = 0;
  size_t totalListItemsPrinted = 0;
  std::string indent;

  void increase_indent() {
    if (options.shouldPrettyPrint()) {
      indent.append(options.prettyIndent, ' ');
    }
  }

  void decrease_indent() {
    if (options.shouldPrettyPrint()) {
      assert(indent.size() >= options.prettyIndent);
      indent.resize(indent.size() - options.prettyIndent);
    }
  }

  /**
   * Print a space (for separating items or attributes).
   *
   * If pretty-printing is enabled, a newline and the current `indent` is
   * printed instead.
   */
  void print_space(bool pretty_print) {
    if (pretty_print) {
      output << "\n" << indent;
    } else {
      output << " ";
    }
  }

  void print_repeated() {
    if (options.ansi_colors)
      output << ANSI_MAGENTA;
    output << "«repeated»";
    if (options.ansi_colors)
      output << ANSI_NORMAL;
  }

  void print_nullptr() {
    if (options.ansi_colors)
      output << ANSI_MAGENTA;
    output << "«nullptr»";
    if (options.ansi_colors)
      output << ANSI_NORMAL;
  }

  void print_elided(unsigned int value, const std::string_view single,
                    const std::string_view plural) {
    ::nix::print_elided(output, value, single, plural, options.ansi_colors);
  }

  void print_int(value_t& v) {
    if (options.ansi_colors)
      output << ANSI_CYAN;
    output << v.integer();
    if (options.ansi_colors)
      output << ANSI_NORMAL;
  }

  void print_float(value_t& v) {
    if (options.ansi_colors)
      output << ANSI_CYAN;
    output << v.fpoint();
    if (options.ansi_colors)
      output << ANSI_NORMAL;
  }

  void print_bool(value_t& v) {
    if (options.ansi_colors)
      output << ANSI_CYAN;
    print_literal_bool(output, v.boolean());
    if (options.ansi_colors)
      output << ANSI_NORMAL;
  }

  void print_string(value_t& v) {
    NixStringContext context;
    copy_context(v, context);
    std::ostringstream oss;
    print_literal_string(oss, v.string_view(), options.maxStringLength, options.ansi_colors);
    output << state.devirtualize(oss.str(), context);
  }

  void print_path(value_t& v) {
    if (options.ansi_colors)
      output << ANSI_GREEN;
    output << v.path().to_string(); // !!! escaping?
    if (options.ansi_colors)
      output << ANSI_NORMAL;
  }

  void print_null() {
    if (options.ansi_colors)
      output << ANSI_CYAN;
    output << "null";
    if (options.ansi_colors)
      output << ANSI_NORMAL;
  }

  void print_derivation(value_t& v) {
    std::optional<store_path_t> store_path;
    if (auto i = v.attrs()->get(state.s.drv_path)) {
      NixStringContext context;
      store_path = state.coerceToStorePath(i->pos, *i->value, context,
                                           "while evaluating the drvPath of a derivation");
    }

    /* This unfortunately breaks printing nested values because of
       how the pretty printer is used (when pretting printing and warning
       to same terminal / std stream). */
#if 0
        if (store_path && !store_path->is_derivation())
            warn(
                "drvPath attribute '%s' is not a valid store path to a derivation, this value not work properly",
                state.store->printStorePath(*store_path));
#endif

    if (options.ansi_colors)
      output << ANSI_GREEN;
    output << "«derivation";
    if (store_path) {
      output << " " << state.store->printStorePath(*store_path);
    }
    output << "»";
    if (options.ansi_colors)
      output << ANSI_NORMAL;
  }

  /**
   * @note This may force items.
   */
  bool should_pretty_print_attrs(AttrVec& v) {
    if (!options.shouldPrettyPrint() || v.empty()) {
      return false;
    }

    // Pretty-print attrsets with more than one item.
    if (v.size() > 1) {
      return true;
    }

    auto item = v[0].second;
    if (!item) {
      return true;
    }

    // It is ok to force the item(s) here, because they will be printed anyway.
    state.forceValue(*item, item->determinePos(no_pos));

    // Pretty-print single-item attrsets only if they contain nested
    // structures.
    auto item_type = item->type();
    return item_type == nList || item_type == nAttrs || item_type == nThunk;
  }

  void print_attrs(value_t& v, size_t depth) {
    if (seen && !seen->insert(v.attrs()).second) {
      print_repeated();
      return;
    }

    if (options.force && options.derivationPaths && state.is_derivation(v)) {
      print_derivation(v);
    } else if (depth < options.max_depth) {
      increase_indent();
      output << "{";

      AttrVec sorted;
      for (auto& i : *v.attrs())
        sorted.emplace_back(std::pair(state.symbols[i.name], i.value));

      if (options.maxAttrs == std::numeric_limits<size_t>::max())
        std::sort(sorted.begin(), sorted.end());
      else
        std::sort(sorted.begin(), sorted.end(), important_first_attr_name_cmp_t());

      auto pretty_print = should_pretty_print_attrs(sorted);

      size_t current_attrs_printed = 0;

      for (auto& i : sorted) {
        print_space(pretty_print);

        if (totalAttrsPrinted >= options.maxAttrs) {
          print_elided(sorted.size() - current_attrs_printed, "attribute", "attributes");
          break;
        }

        print_attribute_name(output, i.first);
        output << " = ";
        print(*i.second, depth + 1);
        output << ";";
        totalAttrsPrinted++;
        current_attrs_printed++;
      }

      decrease_indent();
      print_space(pretty_print);
      output << "}";
    } else {
      output << "{ ... }";
    }
  }

  /**
   * @note This may force items.
   */
  bool should_pretty_print_list(std::span<value_t* const> list) {
    if (!options.shouldPrettyPrint() || list.empty()) {
      return false;
    }

    // Pretty-print lists with more than one item.
    if (list.size() > 1) {
      return true;
    }

    auto item = list[0];
    if (!item) {
      return true;
    }

    // It is ok to force the item(s) here, because they will be printed anyway.
    state.forceValue(*item, item->determinePos(no_pos));

    // Pretty-print single-item lists only if they contain nested
    // structures.
    auto item_type = item->type();
    return item_type == nList || item_type == nAttrs || item_type == nThunk;
  }

  void print_list(value_t& v, size_t depth) {
    if (seen && v.list_size() && !seen->insert(&v).second) {
      print_repeated();
      return;
    }

    if (depth < options.max_depth) {
      increase_indent();
      output << "[";
      auto list_items = v.list_view();
      auto pretty_print = should_pretty_print_list(list_items.span());

      size_t current_list_items_printed = 0;

      for (auto elem : list_items) {
        print_space(pretty_print);

        if (totalListItemsPrinted >= options.maxListItems) {
          print_elided(list_items.size() - current_list_items_printed, "item", "items");
          break;
        }

        if (elem) {
          print(*elem, depth + 1);
        } else {
          print_nullptr();
        }
        totalListItemsPrinted++;
        current_list_items_printed++;
      }

      decrease_indent();
      print_space(pretty_print);
      output << "]";
    } else {
      output << "[ ... ]";
    }
  }

  void print_function(value_t& v) {
    if (options.ansi_colors)
      output << ANSI_BLUE;
    output << "«";

    if (v.isLambda()) {
      output << "lambda";
      if (v.lambda().fun) {
        if (v.lambda().fun->name) {
          output << " " << state.symbols[v.lambda().fun->name];
        }

        output << " @ " << filter_ansi_escapes(state.positions[v.lambda().fun->pos].to_string());
      }
    } else if (v.isPrimOp()) {
      if (v.prim_op())
        output << *v.prim_op();
      else
        output << "primop";
    } else if (v.isPrimOpApp()) {
      output << "partially applied ";
      auto prim_op = v.primOpAppPrimOp();
      if (prim_op)
        output << *prim_op;
      else
        output << "primop";
    } else {
      unreachable();
    }

    output << "»";
    if (options.ansi_colors)
      output << ANSI_NORMAL;
  }

  void print_thunk(value_t& v) {
    if (v.isBlackhole()) {
      // Although we know for sure that it's going to be an infinite recursion
      // when this value is accessed _in the current context_, it's likely
      // that the user will misinterpret a simpler «infinite recursion» output
      // as a definitive statement about the value, while in fact it may be
      // a valid value after `builtins.trace` and perhaps some other steps
      // have completed.
      if (options.ansi_colors)
        output << ANSI_RED;
      output << "«potential infinite recursion»";
      if (options.ansi_colors)
        output << ANSI_NORMAL;
    } else if (!v.isFinished()) {
      if (options.ansi_colors)
        output << ANSI_MAGENTA;
      output << "«thunk»";
      if (options.ansi_colors)
        output << ANSI_NORMAL;
    } else {
      unreachable();
    }
  }

  void print_failed(value_t& v) { output << "«failed»"; }

  void print_external(value_t& v) { v.external()->print(output); }

  void print_unknown() {
    if (options.ansi_colors)
      output << ANSI_RED;
    output << "«unknown»";
    if (options.ansi_colors)
      output << ANSI_NORMAL;
  }

  void print_error_(Error& e) {
    if (options.ansi_colors)
      output << ANSI_RED;
    output << "«error: " << filter_ansi_escapes(e.info().msg_.str(), true) << "»";
    if (options.ansi_colors)
      output << ANSI_NORMAL;
  }

  void print(value_t& v, size_t depth) {
    output.flush();
    check_interrupt();

    try {
      if (options.force) {
        state.forceValue(v, v.determinePos(no_pos));
      }

      switch (v.type()) {
        case nInt:
          print_int(v);
          break;

        case nFloat:
          print_float(v);
          break;

        case nBool:
          print_bool(v);
          break;

        case nString:
          print_string(v);
          break;

        case nPath:
          print_path(v);
          break;

        case nNull:
          print_null();
          break;

        case nAttrs:
          print_attrs(v, depth);
          break;

        case nList:
          print_list(v, depth);
          break;

        case nFunction:
          print_function(v);
          break;

        case nThunk:
          print_thunk(v);
          break;

        case nFailed:
          print_failed(v);
          break;

        case nExternal:
          print_external(v);
          break;

        default:
          print_unknown();
          break;
      }
    } catch (Error& e) {
      if (options.errors == ErrorPrintBehavior::Throw ||
          (options.errors == ErrorPrintBehavior::ThrowTopLevel && depth == 0)) {
        throw;
      }
      print_error_(e);
    }
  }

  printer_t(std::ostream& output, eval_state_t& state, PrintOptions options)
      : output(output), state(state), options(options) {}

  void print(value_t& v) {
    totalAttrsPrinted = 0;
    totalListItemsPrinted = 0;
    indent.clear();

    if (options.trackRepeated) {
      seen.emplace();
    } else {
      seen.reset();
    }

    values_seen_t seen;
    print(v, 0);
  }
};

void print_value(eval_state_t& state, std::ostream& output, value_t& v, PrintOptions options) {
  printer_t(output, state, options).print(v);
}

std::ostream& operator<<(std::ostream& output, const ValuePrinter& printer) {
  print_value(printer.state, output, printer.value, printer.options);
  return output;
}

template <>
hint_fmt_t& hint_fmt_t::operator%(const ValuePrinter& value) {
  fmt_ % value;
  return *this;
}

} // namespace nix
