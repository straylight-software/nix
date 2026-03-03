// straylight::nix::data::json
//
// JSON abstraction layer - isolates backend (nlohmann, simdjson, etc.)
//
// Design goals:
//   - No backend types in public interfaces
//   - Swappable backend without changing call sites
//
// Current backend: nlohmann/json (DOM-based)
// Future: simdjson on-demand for hot paths, dhall for config

#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace straylight::nix::data::json {

// =============================================================================
// Result type for parsing
// =============================================================================

struct parse_error_t {
  std::string message;
  std::size_t offset{0};
};

template <typename T>
struct result_t {
  std::variant<T, parse_error_t> data;

  [[nodiscard]] bool ok() const { return std::holds_alternative<T>(data); }
  [[nodiscard]] bool err() const { return !ok(); }

  [[nodiscard]] T& value() & { return std::get<T>(data); }
  [[nodiscard]] const T& value() const& { return std::get<T>(data); }
  [[nodiscard]] T&& value() && { return std::get<T>(std::move(data)); }

  [[nodiscard]] parse_error_t& error() & { return std::get<parse_error_t>(data); }
  [[nodiscard]] const parse_error_t& error() const& { return std::get<parse_error_t>(data); }
};

// =============================================================================
// JSON value types
// =============================================================================

struct null_t {
  constexpr bool operator==(const null_t&) const = default;
};
inline constexpr null_t null{};

// Forward declare value_t for recursive container definitions
struct value_t;

// Use boxed value_t to allow recursive types in variant
struct boxed_value_t {
  std::unique_ptr<value_t> ptr;

  boxed_value_t();
  explicit boxed_value_t(value_t v);
  boxed_value_t(const boxed_value_t& other);
  boxed_value_t(boxed_value_t&&) noexcept = default;
  boxed_value_t& operator=(const boxed_value_t& other);
  boxed_value_t& operator=(boxed_value_t&&) noexcept = default;
  ~boxed_value_t();

  value_t& operator*() { return *ptr; }
  const value_t& operator*() const { return *ptr; }
  value_t* operator->() { return ptr.get(); }
  const value_t* operator->() const { return ptr.get(); }
};

struct array_t {
  std::vector<boxed_value_t> elements;

  [[nodiscard]] std::size_t size() const { return elements.size(); }
  [[nodiscard]] bool empty() const { return elements.empty(); }

  void push_back(value_t v);
  value_t& operator[](std::size_t i) { return *elements[i]; }
  const value_t& operator[](std::size_t i) const { return *elements[i]; }
};

struct object_t {
  std::vector<std::pair<std::string, boxed_value_t>> members;

  [[nodiscard]] const value_t* get(std::string_view key) const;
  [[nodiscard]] value_t* get(std::string_view key);
  [[nodiscard]] bool contains(std::string_view key) const;
  [[nodiscard]] std::size_t size() const { return members.size(); }
  [[nodiscard]] bool empty() const { return members.empty(); }

  void insert(std::string key, value_t value);
  void insert_or_assign(std::string key, value_t value);
};

struct value_t {
  using variant_type = std::variant<null_t, bool, std::int64_t, std::uint64_t, double, std::string,
                                    array_t, object_t>;
  variant_type data;

  value_t() : data(null) {}
  value_t(null_t) : data(null) {}
  value_t(bool b) : data(b) {}
  value_t(std::int64_t i) : data(i) {}
  value_t(std::uint64_t u) : data(u) {}
  value_t(int i) : data(static_cast<std::int64_t>(i)) {}
  value_t(double d) : data(d) {}
  value_t(std::string s) : data(std::move(s)) {}
  value_t(const char* s) : data(std::string(s)) {}
  value_t(std::string_view s) : data(std::string(s)) {}
  value_t(array_t a) : data(std::move(a)) {}
  value_t(object_t o) : data(std::move(o)) {}

  [[nodiscard]] bool is_null() const { return std::holds_alternative<null_t>(data); }
  [[nodiscard]] bool is_bool() const { return std::holds_alternative<bool>(data); }
  [[nodiscard]] bool is_int() const { return std::holds_alternative<std::int64_t>(data); }
  [[nodiscard]] bool is_uint() const { return std::holds_alternative<std::uint64_t>(data); }
  [[nodiscard]] bool is_number() const { return is_int() || is_uint() || is_double(); }
  [[nodiscard]] bool is_double() const { return std::holds_alternative<double>(data); }
  [[nodiscard]] bool is_string() const { return std::holds_alternative<std::string>(data); }
  [[nodiscard]] bool is_array() const { return std::holds_alternative<array_t>(data); }
  [[nodiscard]] bool is_object() const { return std::holds_alternative<object_t>(data); }

  [[nodiscard]] bool as_bool() const { return std::get<bool>(data); }
  [[nodiscard]] std::int64_t as_int() const { return std::get<std::int64_t>(data); }
  [[nodiscard]] std::uint64_t as_uint() const { return std::get<std::uint64_t>(data); }
  [[nodiscard]] double as_double() const;
  [[nodiscard]] const std::string& as_string() const { return std::get<std::string>(data); }
  [[nodiscard]] std::string& as_string() { return std::get<std::string>(data); }
  [[nodiscard]] const array_t& as_array() const { return std::get<array_t>(data); }
  [[nodiscard]] array_t& as_array() { return std::get<array_t>(data); }
  [[nodiscard]] const object_t& as_object() const { return std::get<object_t>(data); }
  [[nodiscard]] object_t& as_object() { return std::get<object_t>(data); }

  [[nodiscard]] std::optional<bool> get_bool() const;
  [[nodiscard]] std::optional<std::int64_t> get_int() const;
  [[nodiscard]] std::optional<std::uint64_t> get_uint() const;
  [[nodiscard]] std::optional<double> get_double() const;
  [[nodiscard]] std::optional<std::string_view> get_string() const;
  [[nodiscard]] const array_t* get_array() const;
  [[nodiscard]] const object_t* get_object() const;

  [[nodiscard]] const value_t* get(std::string_view key) const;
  [[nodiscard]] const value_t* get(std::size_t index) const;
};

// =============================================================================
// Parsing and serialization
// =============================================================================

[[nodiscard]] result_t<value_t> parse(std::string_view input);
[[nodiscard]] result_t<value_t> parse(std::span<const std::byte> input);
[[nodiscard]] std::string to_string(const value_t& value);
[[nodiscard]] std::string to_string_pretty(const value_t& value, int indent = 2);

} // namespace straylight::nix::data::json
