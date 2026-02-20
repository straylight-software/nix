#pragma once
/// @file nix-language/eval/value.h
/// Runtime values for the Nix interpreter.

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <variant>
#include <vector>

#include "straylight/language/ast/expression.h"
#include "straylight/language/ast/symbol_table.h"

namespace straylight::language::eval {

// Forward declarations
struct value;
using value_ptr = std::shared_ptr<value>;
struct environment;
using env_ptr = std::shared_ptr<environment>;

/// A thunk - unevaluated expression with its environment
struct thunk {
  const ast::expression* expr; // non-owning pointer to expression
  env_ptr env;
  mutable value_ptr cached;        // memoized result
  mutable bool evaluating = false; // cycle detection
};

/// A closure - lambda with captured environment
struct closure {
  const ast::expression_lambda* lambda;
  env_ptr env;
};

/// An attribute set
struct attr_set {
  std::unordered_map<std::string, value_ptr> attrs;
};

/// A list
struct value_list {
  std::vector<value_ptr> elements;
};

/// A path
struct value_path {
  std::string path;
};

/// A builtin function
using builtin_func = std::function<value_ptr(std::vector<value_ptr>&)>;
struct builtin {
  std::string name;
  std::size_t arity;
  builtin_func func;
  std::vector<value_ptr> applied_args; // for partial application
};

/// Null value type (to avoid std::nullptr_t issues)
struct value_null {};

/// The value variant
using value_variant = std::variant<value_null,   // null
                                   bool,         // boolean
                                   std::int64_t, // integer
                                   double,       // float
                                   std::string,  // string
                                   value_path,   // path
                                   value_list,   // list
                                   attr_set,     // attribute set
                                   closure,      // function closure
                                   builtin,      // builtin function
                                   thunk         // unevaluated expression
                                   >;

/// A Nix value
struct value {
  value_variant data;

  explicit value(value_variant v) : data(std::move(v)) {}

  // Convenience constructors
  static auto make_null() -> value_ptr { return std::make_shared<value>(value_null{}); }
  static auto make_bool(bool b) -> value_ptr { return std::make_shared<value>(b); }
  static auto make_int(std::int64_t i) -> value_ptr { return std::make_shared<value>(i); }
  static auto make_float(double d) -> value_ptr { return std::make_shared<value>(d); }
  static auto make_string(std::string s) -> value_ptr {
    return std::make_shared<value>(std::move(s));
  }
  static auto make_path(std::string p) -> value_ptr {
    return std::make_shared<value>(value_path{std::move(p)});
  }
  static auto make_list(std::vector<value_ptr> elems) -> value_ptr {
    return std::make_shared<value>(value_list{std::move(elems)});
  }
  static auto make_attrs(std::unordered_map<std::string, value_ptr> attrs) -> value_ptr {
    return std::make_shared<value>(attr_set{std::move(attrs)});
  }
  static auto make_closure(const ast::expression_lambda* lambda, env_ptr env) -> value_ptr {
    return std::make_shared<value>(closure{lambda, std::move(env)});
  }
  static auto make_thunk(const ast::expression& expr, env_ptr env) -> value_ptr {
    return std::make_shared<value>(thunk{&expr, std::move(env), nullptr, false});
  }
  static auto make_builtin(std::string name, std::size_t arity, builtin_func func) -> value_ptr {
    return std::make_shared<value>(builtin{std::move(name), arity, std::move(func), {}});
  }
};

/// Environment - variable bindings
struct environment {
  env_ptr parent;
  std::unordered_map<std::string, value_ptr> bindings;

  explicit environment(env_ptr p = nullptr) : parent(std::move(p)) {}

  auto lookup(std::string_view name) const -> value_ptr {
    auto it = bindings.find(std::string(name));
    if (it != bindings.end()) {
      return it->second;
    }
    if (parent) {
      return parent->lookup(name);
    }
    return nullptr;
  }

  void bind(std::string_view name, value_ptr val) { bindings[std::string(name)] = std::move(val); }
};

/// Type checking helpers
inline auto is_null(const value_ptr& v) -> bool {
  return std::holds_alternative<value_null>(v->data);
}
inline auto is_bool(const value_ptr& v) -> bool {
  return std::holds_alternative<bool>(v->data);
}
inline auto is_int(const value_ptr& v) -> bool {
  return std::holds_alternative<std::int64_t>(v->data);
}
inline auto is_float(const value_ptr& v) -> bool {
  return std::holds_alternative<double>(v->data);
}
inline auto is_string(const value_ptr& v) -> bool {
  return std::holds_alternative<std::string>(v->data);
}
inline auto is_path(const value_ptr& v) -> bool {
  return std::holds_alternative<value_path>(v->data);
}
inline auto is_list(const value_ptr& v) -> bool {
  return std::holds_alternative<value_list>(v->data);
}
inline auto is_attrs(const value_ptr& v) -> bool {
  return std::holds_alternative<attr_set>(v->data);
}
inline auto is_closure(const value_ptr& v) -> bool {
  return std::holds_alternative<closure>(v->data);
}
inline auto is_builtin(const value_ptr& v) -> bool {
  return std::holds_alternative<builtin>(v->data);
}
inline auto is_thunk(const value_ptr& v) -> bool {
  return std::holds_alternative<thunk>(v->data);
}

/// Get typed values (assumes already checked)
inline auto as_bool(const value_ptr& v) -> bool {
  return std::get<bool>(v->data);
}
inline auto as_int(const value_ptr& v) -> std::int64_t {
  return std::get<std::int64_t>(v->data);
}
inline auto as_float(const value_ptr& v) -> double {
  return std::get<double>(v->data);
}
inline auto as_string(const value_ptr& v) -> const std::string& {
  return std::get<std::string>(v->data);
}
inline auto as_path(const value_ptr& v) -> const value_path& {
  return std::get<value_path>(v->data);
}
inline auto as_list(const value_ptr& v) -> const value_list& {
  return std::get<value_list>(v->data);
}
inline auto as_attrs(const value_ptr& v) -> const attr_set& {
  return std::get<attr_set>(v->data);
}
inline auto as_closure(const value_ptr& v) -> const closure& {
  return std::get<closure>(v->data);
}
inline auto as_builtin(const value_ptr& v) -> builtin& {
  return std::get<builtin>(v->data);
}
inline auto as_thunk(const value_ptr& v) -> thunk& {
  return std::get<thunk>(v->data);
}

} // namespace straylight::language::eval
