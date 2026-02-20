#pragma once
///@file

#include <memory_resource>

#include <boost/unordered/concurrent_flat_set.hpp>
#include <boost/version.hpp>

#include "nix/expr/value.h"
#include "nix/util/alignment.h"
#include "nix/util/error.h"
#include "nix/util/sync.h"

namespace nix {

class SymbolValue : protected value_t {
  friend class SymbolStr;
  friend class symbol_table_t;

  operator std::string_view() const noexcept { return string_view(); }
};

struct ContiguousArena {
  const char* data;
  const size_t max_size;

  // Put this in a separate cache line to ensure that a thread
  // adding a symbol doesn't slow down threads dereferencing symbols
  // by invalidating the read-only `data` field.
  alignas(64) std::atomic<size_t> size{0};

  ContiguousArena(size_t max_size);

  size_t allocate(size_t bytes);
};

class StaticSymbolTable;

/**
 * Symbols have the property that they can be compared efficiently
 * (using an equality test), because the symbol table stores only one
 * copy of each string.
 */
class symbol_t {
  friend class SymbolStr;
  friend class symbol_table_t;
  friend class StaticSymbolTable;

private:
  /// The offset of the symbol in `symbol_table_t::arena`.
  uint32_t id;

  explicit constexpr symbol_t(uint32_t id) noexcept : id(id) {}

public:
  constexpr symbol_t() noexcept : id(0) {}

  [[gnu::always_inline]]
  constexpr explicit operator bool() const noexcept {
    return id > 0;
  }

  /**
   * The ID is a private implementation detail that should generally not be observed. However, we
   * expose here just for sake of `switch...case`, which needs to dispatch on numbers. */
  [[gnu::always_inline]]
  constexpr uint32_t getId() const noexcept {
    return id;
  }

  constexpr auto operator<=>(const symbol_t& other) const noexcept = default;

  friend class std::hash<symbol_t>;

  constexpr static size_t alignment = alignof(SymbolValue);
};

/**
 * This class mainly exists to give us an operator<< for ostreams. We could also
 * return plain strings from symbol_table_t, but then we'd have to wrap every
 * instance of a symbol that is fmt()ed, which is inconvenient and error-prone.
 */
class SymbolStr {
  friend class symbol_table_t;

  const SymbolValue* s;

  struct Key {
    using hash_type_t = boost::hash<std::string_view>;

    std::string_view s;
    std::size_t hash;
    ContiguousArena& arena;

    Key(std::string_view s, ContiguousArena& arena) : s(s), hash(hash_type_t{}(s)), arena(arena) {}
  };

public:
  SymbolStr(const SymbolValue& s) noexcept : s(&s) {}

  SymbolStr(const Key& key);

  bool operator==(std::string_view s2) const noexcept { return *s == s2; }

  [[gnu::always_inline]]
  const StringData& string_data() const noexcept {
    return s->string_data();
  }

  [[gnu::always_inline]]
  const char* c_str() const noexcept {
    return s->c_str();
  }

  [[gnu::always_inline]] operator std::string_view() const noexcept { return *s; }

  friend std::ostream& operator<<(std::ostream& os, const SymbolStr& symbol);

  [[gnu::always_inline]]
  bool empty() const noexcept {
    return !s->string_data().size();
  }

  [[gnu::always_inline]]
  size_t size() const noexcept {
    return s->string_data().size();
  }

  [[gnu::always_inline]]
  const value_t* valuePtr() const noexcept {
    return s;
  }

  struct Hash {
    using is_transparent = void;
    using is_avalanching = std::true_type;

    std::size_t operator()(SymbolStr str) const { return Key::hash_type_t{}(*str.s); }

    std::size_t operator()(const Key& key) const noexcept { return key.hash; }
  };

  struct Equal {
    using is_transparent = void;

    bool operator()(SymbolStr a, SymbolStr b) const noexcept {
      // strings are unique, so that a pointer comparison is OK
      return a.s == b.s;
    }

    bool operator()(SymbolStr a, const Key& b) const noexcept { return a == b.s; }

    [[gnu::always_inline]]
    bool operator()(const Key& a, SymbolStr b) const noexcept {
      return operator()(b, a);
    }
  };

  constexpr static size_t computeSize(std::string_view s) {
    return align_up(sizeof(value_t) + sizeof(StringData) + s.size() + 1, symbol_t::alignment);
  }
};

class symbol_table_t;

/**
 * Convenience class to statically assign symbol identifiers at compile-time.
 */
class StaticSymbolTable {
  static constexpr std::size_t max_size = 1024;

  struct StaticSymbolInfo {
    std::string_view str;
    symbol_t sym;
  };

  std::array<StaticSymbolInfo, max_size> symbols;
  std::size_t size = 0;
  std::size_t next_id = alignof(SymbolValue);

public:
  constexpr StaticSymbolTable() = default;

  constexpr symbol_t create(std::string_view str) {
    /* No need to check bounds because out of bounds access is
       a compilation error. */
    auto sym = symbol_t(next_id);
    symbols[size++] = {str, sym};
    next_id += SymbolStr::computeSize(str);
    return sym;
  }

  void copyIntoSymbolTable(symbol_table_t& symtab) const;
};

/**
 * symbol_t table used by the parser and evaluator to represent and look
 * up identifiers and attributes efficiently.
 */
class symbol_table_t {
private:
  /**
   * symbol_table_t is an append only data structure.
   * During its lifetime the monotonic buffer holds all strings and nodes, if the symbol set is node
   * based.
   */
  ContiguousArena arena;

  /**
   * Transparent lookup of string view for a pointer to a
   * SymbolValue in the arena.
   */
  boost::concurrent_flat_set<SymbolStr, SymbolStr::Hash, SymbolStr::Equal> symbols;

public:
  symbol_table_t(const StaticSymbolTable& staticSymtab) : arena(1 << 30) {
    // Reserve symbol ID 0 and ensure alignment of the first allocation.
    arena.allocate(symbol_t::alignment);

    staticSymtab.copyIntoSymbolTable(*this);
  }

  /**
   * Converts a string into a symbol.
   */
  symbol_t create(std::string_view s);

  std::vector<SymbolStr> resolve(const std::span<const symbol_t>& symbols) const {
    std::vector<SymbolStr> result;
    result.reserve(symbols.size());
    for (auto& sym : symbols)
      result.push_back((*this)[sym]);
    return result;
  }

  SymbolStr operator[](symbol_t s) const {
    assert(s.id);
    // Note: we don't check arena.size here to avoid a dependency
    // on other threads creating new symbols.
    return SymbolStr(*reinterpret_cast<const SymbolValue*>(arena.data + s.id));
  }

  size_t size() const noexcept { return symbols.size(); }

  size_t totalSize() const { return arena.size; }

  template <typename T>
  void dump(T callback) const {
    std::string_view left{arena.data, arena.size};
    left = left.substr(symbol_t::alignment);
    while (!left.empty()) {
      auto v = reinterpret_cast<const SymbolValue*>(left.data());
      callback(v->string_view());
      left = left.substr(
          align_up(sizeof(SymbolValue) + sizeof(StringData) + v->string_view().size() + 1,
                   symbol_t::alignment));
    }
  }
};

inline void StaticSymbolTable::copyIntoSymbolTable(symbol_table_t& symtab) const {
  for (std::size_t i = 0; i < size; ++i) {
    auto [str, staticSym] = symbols[i];
    auto sym = symtab.create(str);
    if (sym != staticSym) [[unlikely]]
      unreachable();
  }
}

} // namespace nix

template <>
struct std::hash<nix::symbol_t> {
  std::size_t operator()(const nix::symbol_t& s) const noexcept {
    return std::hash<decltype(s.id)>{}(s.id);
  }
};
