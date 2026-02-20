#pragma once
///@file straylight/nix/compiler/ast/symbol_table.h
/// Symbol interning for efficient identifier comparison.

#include <cstdint>
#include <deque>
#include <string>
#include <string_view>
#include <unordered_map>

#include "expression.h"

namespace straylight::nix::compiler::ast {

/// symbol table for interning identifiers and attribute names
/// thread-safe for concurrent reads after construction, but not for writes
class symbol_table {
public:
  symbol_table() = default;

  // non-copyable, non-movable (symbols reference internal storage)
  symbol_table(const symbol_table&) = delete;
  symbol_table(symbol_table&&) = delete;
  auto operator=(const symbol_table&) -> symbol_table& = delete;
  auto operator=(symbol_table&&) -> symbol_table& = delete;
  ~symbol_table() = default;

  /// intern a string, returning a symbol handle
  /// if the string is already interned, returns the existing symbol
  [[nodiscard]] auto intern(std::string_view text) -> symbol {
    if (auto iterator = index_.find(text); iterator != index_.end()) {
      return symbol{iterator->second};
    }

    auto symbol_index = static_cast<std::uint32_t>(strings_.size());
    // Use deque for stable references - iterators/references to elements
    // remain valid after push_back (unlike vector which may reallocate)
    strings_.emplace_back(text);
    index_.emplace(strings_.back(), symbol_index);
    return symbol{symbol_index};
  }

  /// look up the string for a symbol
  [[nodiscard]] auto lookup(symbol sym) const noexcept -> std::string_view {
    return strings_[sym.index_];
  }

  /// number of interned symbols
  [[nodiscard]] auto size() const noexcept -> std::size_t { return strings_.size(); }

  // well-known symbols (initialized lazily)
  [[nodiscard]] auto symbol_or() -> symbol { return intern("or"); }
  [[nodiscard]] auto symbol_true() -> symbol { return intern("true"); }
  [[nodiscard]] auto symbol_false() -> symbol { return intern("false"); }
  [[nodiscard]] auto symbol_null() -> symbol { return intern("null"); }
  [[nodiscard]] auto symbol_body() -> symbol { return intern("body"); }
  [[nodiscard]] auto symbol_curpos() -> symbol { return intern("__curPos"); }
  [[nodiscard]] auto symbol_overrides() -> symbol { return intern("__overrides"); }
  [[nodiscard]] auto symbol_nix_path() -> symbol { return intern("__nixPath"); }
  [[nodiscard]] auto symbol_find_file() -> symbol { return intern("__findFile"); }

  // builtins for desugaring operators
  [[nodiscard]] auto symbol_less_than() -> symbol { return intern("__lessThan"); }
  [[nodiscard]] auto symbol_sub() -> symbol { return intern("__sub"); }
  [[nodiscard]] auto symbol_mul() -> symbol { return intern("__mul"); }
  [[nodiscard]] auto symbol_div() -> symbol { return intern("__div"); }

private:
  // deque provides stable references - strings don't move when new ones are added
  // this allows string_view keys in index_ to remain valid
  std::deque<std::string> strings_;
  std::unordered_map<std::string_view, std::uint32_t> index_;
};

} // namespace straylight::nix::compiler::ast
