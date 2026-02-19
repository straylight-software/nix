#pragma once
/// @file nix-language/eval/eval.hh
/// Tree-walking interpreter for Nix expressions.

#include <stdexcept>
#include <string>

#include "nix-language/ast/expression.hh"
#include "nix-language/ast/symbol_table.hh"
#include "nix-language/eval/value.hh"

namespace nix::language::eval {

/// Evaluation error
class eval_error : public std::runtime_error {
public:
  using std::runtime_error::runtime_error;
};

/// The interpreter
class evaluator {
public:
  explicit evaluator(ast::symbol_table& symbols) : symbols_(symbols) {
    root_env_ = std::make_shared<environment>();
    setup_builtins();
  }

  /// Evaluate an expression
  auto eval(const ast::expression& expr) -> value_ptr { return eval_expr(expr, root_env_); }

  /// Force a value (evaluate thunks)
  auto force(value_ptr val) -> value_ptr {
    while (is_thunk(val)) {
      auto& t = as_thunk(val);
      if (t.cached) {
        val = t.cached;
        continue;
      }
      if (t.evaluating) {
        throw eval_error("infinite recursion detected");
      }
      t.evaluating = true;
      t.cached = eval_expr(*t.expr, t.env);
      t.evaluating = false;
      val = t.cached;
    }
    return val;
  }

  /// Convert value to string for display
  auto print_value(const value_ptr& val) -> std::string {
    auto v = force(val);
    return std::visit(
        [this](const auto& x) -> std::string {
          using T = std::decay_t<decltype(x)>;
          if constexpr (std::is_same_v<T, value_null>) {
            return "null";
          } else if constexpr (std::is_same_v<T, bool>) {
            return x ? "true" : "false";
          } else if constexpr (std::is_same_v<T, std::int64_t>) {
            return std::to_string(x);
          } else if constexpr (std::is_same_v<T, double>) {
            return std::to_string(x);
          } else if constexpr (std::is_same_v<T, std::string>) {
            return "\"" + x + "\"";
          } else if constexpr (std::is_same_v<T, value_path>) {
            return x.path;
          } else if constexpr (std::is_same_v<T, value_list>) {
            std::string result = "[ ";
            for (const auto& elem : x.elements) {
              result += print_value(elem) + " ";
            }
            result += "]";
            return result;
          } else if constexpr (std::is_same_v<T, attr_set>) {
            std::string result = "{ ";
            for (const auto& [k, v] : x.attrs) {
              result += k + " = " + print_value(v) + "; ";
            }
            result += "}";
            return result;
          } else if constexpr (std::is_same_v<T, closure>) {
            return "<lambda>";
          } else if constexpr (std::is_same_v<T, builtin>) {
            return "<builtin:" + x.name + ">";
          } else if constexpr (std::is_same_v<T, thunk>) {
            return "<thunk>";
          } else {
            return "<unknown>";
          }
        },
        v->data);
  }

private:
  ast::symbol_table& symbols_;
  env_ptr root_env_;

  void setup_builtins() {
    // Basic builtins
    root_env_->bind("true", value::make_bool(true));
    root_env_->bind("false", value::make_bool(false));
    root_env_->bind("null", value::make_null());

    // builtins attrset
    std::unordered_map<std::string, value_ptr> builtins_map;

    // builtins.add
    builtins_map["add"] = value::make_builtin("add", 2, [this](std::vector<value_ptr>& args) {
      auto a = force(args[0]);
      auto b = force(args[1]);
      if (is_int(a) && is_int(b)) {
        return value::make_int(as_int(a) + as_int(b));
      }
      if (is_float(a) || is_float(b)) {
        double av = is_int(a) ? static_cast<double>(as_int(a)) : as_float(a);
        double bv = is_int(b) ? static_cast<double>(as_int(b)) : as_float(b);
        return value::make_float(av + bv);
      }
      throw eval_error("cannot add non-numeric values");
    });

    // builtins.sub
    builtins_map["sub"] = value::make_builtin("sub", 2, [this](std::vector<value_ptr>& args) {
      auto a = force(args[0]);
      auto b = force(args[1]);
      if (is_int(a) && is_int(b)) {
        return value::make_int(as_int(a) - as_int(b));
      }
      throw eval_error("cannot subtract non-numeric values");
    });

    // builtins.mul
    builtins_map["mul"] = value::make_builtin("mul", 2, [this](std::vector<value_ptr>& args) {
      auto a = force(args[0]);
      auto b = force(args[1]);
      if (is_int(a) && is_int(b)) {
        return value::make_int(as_int(a) * as_int(b));
      }
      throw eval_error("cannot multiply non-numeric values");
    });

    // builtins.div
    builtins_map["div"] = value::make_builtin("div", 2, [this](std::vector<value_ptr>& args) {
      auto a = force(args[0]);
      auto b = force(args[1]);
      if (is_int(a) && is_int(b)) {
        if (as_int(b) == 0)
          throw eval_error("division by zero");
        return value::make_int(as_int(a) / as_int(b));
      }
      throw eval_error("cannot divide non-numeric values");
    });

    // builtins.head
    builtins_map["head"] = value::make_builtin("head", 1, [this](std::vector<value_ptr>& args) {
      auto lst = force(args[0]);
      if (!is_list(lst))
        throw eval_error("head: expected list");
      const auto& l = as_list(lst);
      if (l.elements.empty())
        throw eval_error("head: empty list");
      return l.elements[0];
    });

    // builtins.tail
    builtins_map["tail"] = value::make_builtin("tail", 1, [this](std::vector<value_ptr>& args) {
      auto lst = force(args[0]);
      if (!is_list(lst))
        throw eval_error("tail: expected list");
      const auto& l = as_list(lst);
      if (l.elements.empty())
        throw eval_error("tail: empty list");
      std::vector<value_ptr> rest(l.elements.begin() + 1, l.elements.end());
      return value::make_list(std::move(rest));
    });

    // builtins.length
    builtins_map["length"] = value::make_builtin("length", 1, [this](std::vector<value_ptr>& args) {
      auto lst = force(args[0]);
      if (!is_list(lst))
        throw eval_error("length: expected list");
      return value::make_int(static_cast<std::int64_t>(as_list(lst).elements.size()));
    });

    // builtins.attrNames
    builtins_map["attrNames"] =
        value::make_builtin("attrNames", 1, [this](std::vector<value_ptr>& args) {
          auto attrs = force(args[0]);
          if (!is_attrs(attrs))
            throw eval_error("attrNames: expected attrset");
          std::vector<value_ptr> names;
          for (const auto& [k, v] : as_attrs(attrs).attrs) {
            names.push_back(value::make_string(k));
          }
          return value::make_list(std::move(names));
        });

    // builtins.hasAttr
    builtins_map["hasAttr"] =
        value::make_builtin("hasAttr", 2, [this](std::vector<value_ptr>& args) {
          auto name = force(args[0]);
          auto attrs = force(args[1]);
          if (!is_string(name))
            throw eval_error("hasAttr: first arg must be string");
          if (!is_attrs(attrs))
            throw eval_error("hasAttr: second arg must be attrset");
          return value::make_bool(as_attrs(attrs).attrs.count(as_string(name)) > 0);
        });

    // builtins.getAttr
    builtins_map["getAttr"] =
        value::make_builtin("getAttr", 2, [this](std::vector<value_ptr>& args) {
          auto name = force(args[0]);
          auto attrs = force(args[1]);
          if (!is_string(name))
            throw eval_error("getAttr: first arg must be string");
          if (!is_attrs(attrs))
            throw eval_error("getAttr: second arg must be attrset");
          auto it = as_attrs(attrs).attrs.find(as_string(name));
          if (it == as_attrs(attrs).attrs.end()) {
            throw eval_error("attribute '" + as_string(name) + "' not found");
          }
          return it->second;
        });

    // builtins.typeOf
    builtins_map["typeOf"] = value::make_builtin("typeOf", 1, [this](std::vector<value_ptr>& args) {
      auto v = force(args[0]);
      if (is_bool(v))
        return value::make_string("bool");
      if (is_int(v))
        return value::make_string("int");
      if (is_float(v))
        return value::make_string("float");
      if (is_string(v))
        return value::make_string("string");
      if (is_path(v))
        return value::make_string("path");
      if (is_list(v))
        return value::make_string("list");
      if (is_attrs(v))
        return value::make_string("set");
      if (is_closure(v) || is_builtin(v))
        return value::make_string("lambda");
      return value::make_string("null");
    });

    // builtins.toString
    builtins_map["toString"] =
        value::make_builtin("toString", 1, [this](std::vector<value_ptr>& args) {
          auto v = force(args[0]);
          if (is_string(v))
            return v;
          if (is_int(v))
            return value::make_string(std::to_string(as_int(v)));
          if (is_float(v))
            return value::make_string(std::to_string(as_float(v)));
          if (is_bool(v))
            return value::make_string(as_bool(v) ? "1" : "");
          if (is_path(v))
            return value::make_string(as_path(v).path);
          throw eval_error("cannot coerce value to string");
        });

    // builtins.map
    builtins_map["map"] = value::make_builtin("map", 2, [this](std::vector<value_ptr>& args) {
      auto func = args[0];
      auto lst = force(args[1]);
      if (!is_list(lst))
        throw eval_error("map: second arg must be list");
      std::vector<value_ptr> result;
      for (const auto& elem : as_list(lst).elements) {
        result.push_back(apply(func, elem));
      }
      return value::make_list(std::move(result));
    });

    // builtins.filter
    builtins_map["filter"] = value::make_builtin("filter", 2, [this](std::vector<value_ptr>& args) {
      auto pred = args[0];
      auto lst = force(args[1]);
      if (!is_list(lst))
        throw eval_error("filter: second arg must be list");
      std::vector<value_ptr> result;
      for (const auto& elem : as_list(lst).elements) {
        auto cond = force(apply(pred, elem));
        if (is_bool(cond) && as_bool(cond)) {
          result.push_back(elem);
        }
      }
      return value::make_list(std::move(result));
    });

    // builtins.foldl'
    builtins_map["foldl'"] = value::make_builtin("foldl'", 3, [this](std::vector<value_ptr>& args) {
      auto func = args[0];
      auto init = force(args[1]);
      auto lst = force(args[2]);
      if (!is_list(lst))
        throw eval_error("foldl': third arg must be list");
      auto acc = init;
      for (const auto& elem : as_list(lst).elements) {
        auto f1 = apply(func, acc);
        acc = force(apply(f1, elem));
      }
      return acc;
    });

    // builtins.abort
    builtins_map["abort"] =
        value::make_builtin("abort", 1, [this](std::vector<value_ptr>& args) -> value_ptr {
          auto msg = force(args[0]);
          if (is_string(msg)) {
            throw eval_error("evaluation aborted: " + as_string(msg));
          }
          throw eval_error("evaluation aborted");
        });

    // builtins.throw
    builtins_map["throw"] =
        value::make_builtin("throw", 1, [this](std::vector<value_ptr>& args) -> value_ptr {
          auto msg = force(args[0]);
          if (is_string(msg)) {
            throw eval_error(as_string(msg));
          }
          throw eval_error("error thrown");
        });

    root_env_->bind("builtins", value::make_attrs(std::move(builtins_map)));
  }

  auto eval_expr(const ast::expression& expr, env_ptr env) -> value_ptr {
    return std::visit([this, &env](const auto& e) { return eval_variant(e, env); }, expr->data);
  }

  // Expression evaluators
  auto eval_variant(const ast::expression_integer& expr, env_ptr) -> value_ptr {
    return value::make_int(expr.value);
  }

  auto eval_variant(const ast::expression_float& expr, env_ptr) -> value_ptr {
    return value::make_float(expr.value);
  }

  auto eval_variant(const ast::expression_string& expr, env_ptr) -> value_ptr {
    return value::make_string(expr.value);
  }

  auto eval_variant(const ast::expression_string_interpolated& expr, env_ptr env) -> value_ptr {
    std::string result;
    for (const auto& part : expr.parts) {
      if (auto* s = std::get_if<std::string>(&part)) {
        result += *s;
      } else {
        auto& e = std::get<ast::expression>(part);
        auto v = force(eval_expr(e, env));
        if (is_string(v)) {
          result += as_string(v);
        } else if (is_int(v)) {
          result += std::to_string(as_int(v));
        } else if (is_path(v)) {
          result += as_path(v).path;
        } else {
          throw eval_error("cannot interpolate value into string");
        }
      }
    }
    return value::make_string(result);
  }

  auto eval_variant(const ast::expression_path& expr, env_ptr) -> value_ptr {
    return value::make_path(expr.value);
  }

  auto eval_variant(const ast::expression_path_interpolated& expr, env_ptr env) -> value_ptr {
    std::string result;
    for (const auto& part : expr.parts) {
      if (auto* s = std::get_if<std::string>(&part)) {
        result += *s;
      } else {
        auto& e = std::get<ast::expression>(part);
        auto v = force(eval_expr(e, env));
        if (is_string(v)) {
          result += as_string(v);
        } else if (is_path(v)) {
          result += as_path(v).path;
        } else {
          throw eval_error("cannot interpolate value into path");
        }
      }
    }
    return value::make_path(result);
  }

  auto eval_variant(const ast::expression_identifier& expr, env_ptr env) -> value_ptr {
    auto name = symbols_.lookup(expr.name);
    auto val = env->lookup(name);
    if (!val) {
      throw eval_error("undefined variable: " + std::string(name));
    }
    return val;
  }

  auto eval_variant(const ast::expression_list& expr, env_ptr env) -> value_ptr {
    std::vector<value_ptr> elements;
    elements.reserve(expr.elements.size());
    for (const auto& elem : expr.elements) {
      // Lists are lazy - create thunks
      elements.push_back(value::make_thunk(elem, env));
    }
    return value::make_list(std::move(elements));
  }

  // Helper to insert a value at a nested path, creating intermediate attrsets as needed
  void insert_nested_attr(std::unordered_map<std::string, value_ptr>& attrs,
                          const std::vector<std::string>& path, value_ptr value) {
    if (path.empty())
      return;

    if (path.size() == 1) {
      attrs[path[0]] = value;
      return;
    }

    // Need to create or merge with nested attrset
    const std::string& first = path[0];
    std::vector<std::string> rest(path.begin() + 1, path.end());

    if (auto it = attrs.find(first); it != attrs.end()) {
      // Existing entry - must be an attrset to merge into
      auto existing = force(it->second);
      if (!is_attrs(existing)) {
        throw eval_error("attribute '" + first + "' already defined as non-attrset");
      }
      // Copy the existing attrs and add to them
      auto nested = as_attrs(existing).attrs;
      insert_nested_attr(nested, rest, value);
      attrs[first] = value::make_attrs(std::move(nested));
    } else {
      // Create new nested attrset
      std::unordered_map<std::string, value_ptr> nested;
      insert_nested_attr(nested, rest, value);
      attrs[first] = value::make_attrs(std::move(nested));
    }
  }

  auto eval_variant(const ast::expression_attribute_set& expr, env_ptr env) -> value_ptr {
    auto attrs_env = expr.is_recursive ? std::make_shared<environment>(env) : env;
    std::unordered_map<std::string, value_ptr> attrs;

    // First pass: create thunks for all bindings
    for (const auto& binding_var : expr.bindings) {
      if (auto* b = std::get_if<ast::binding_attribute>(&binding_var)) {
        // Extract path segments as strings
        std::vector<std::string> path;
        for (const auto& seg : b->path.segments) {
          if (auto* sym = std::get_if<ast::symbol>(&seg.value)) {
            path.emplace_back(symbols_.lookup(*sym));
          } else {
            // Dynamic attribute - evaluate it
            auto& dyn = std::get<ast::expression>(seg.value);
            auto key = force(eval_expr(dyn, env));
            if (!is_string(key)) {
              throw eval_error("dynamic attribute key must be string");
            }
            path.emplace_back(as_string(key));
          }
        }

        if (!path.empty()) {
          auto thunk_env = expr.is_recursive ? attrs_env : env;
          auto thunk = value::make_thunk(b->value, thunk_env);
          insert_nested_attr(attrs, path, thunk);
        }
      } else if (auto* inh = std::get_if<ast::binding_inherit>(&binding_var)) {
        if (inh->from_expression) {
          auto src = force(eval_expr(*inh->from_expression, env));
          if (!is_attrs(src))
            throw eval_error("inherit source must be attrset");
          for (const auto& attr : inh->attributes) {
            auto* sym = std::get_if<ast::symbol>(&attr.value);
            if (!sym)
              throw eval_error("dynamic attribute in inherit");
            std::string name(symbols_.lookup(*sym));
            auto it = as_attrs(src).attrs.find(name);
            if (it == as_attrs(src).attrs.end()) {
              throw eval_error("inherited attribute '" + name + "' not found");
            }
            attrs[name] = it->second;
          }
        } else {
          for (const auto& attr : inh->attributes) {
            auto* sym = std::get_if<ast::symbol>(&attr.value);
            if (!sym)
              throw eval_error("dynamic attribute in inherit");
            std::string name(symbols_.lookup(*sym));
            auto val = env->lookup(name);
            if (!val)
              throw eval_error("inherited variable '" + name + "' not found");
            attrs[name] = val;
          }
        }
      }
    }

    // For recursive sets, bind all attrs to the recursive env
    if (expr.is_recursive) {
      for (const auto& [name, val] : attrs) {
        attrs_env->bind(name, val);
      }
    }

    return value::make_attrs(std::move(attrs));
  }

  auto eval_variant(const ast::expression_select& expr, env_ptr env) -> value_ptr {
    auto subject = force(eval_expr(expr.subject, env));

    for (const auto& seg : expr.path.segments) {
      if (!is_attrs(subject)) {
        if (expr.default_value) {
          return eval_expr(*expr.default_value, env);
        }
        throw eval_error("cannot select from non-attrset");
      }

      std::string name;
      if (auto* sym = std::get_if<ast::symbol>(&seg.value)) {
        name = symbols_.lookup(*sym);
      } else {
        auto& dyn = std::get<ast::expression>(seg.value);
        auto key = force(eval_expr(dyn, env));
        if (!is_string(key))
          throw eval_error("dynamic attr key must be string");
        name = as_string(key);
      }

      auto it = as_attrs(subject).attrs.find(name);
      if (it == as_attrs(subject).attrs.end()) {
        if (expr.default_value) {
          return eval_expr(*expr.default_value, env);
        }
        throw eval_error("attribute '" + name + "' not found");
      }
      subject = force(it->second);
    }
    return subject;
  }

  auto eval_variant(const ast::expression_has_attribute& expr, env_ptr env) -> value_ptr {
    auto subject = force(eval_expr(expr.subject, env));

    for (const auto& seg : expr.path.segments) {
      if (!is_attrs(subject)) {
        return value::make_bool(false);
      }

      std::string name;
      if (auto* sym = std::get_if<ast::symbol>(&seg.value)) {
        name = symbols_.lookup(*sym);
      } else {
        auto& dyn = std::get<ast::expression>(seg.value);
        auto key = force(eval_expr(dyn, env));
        if (!is_string(key))
          return value::make_bool(false);
        name = as_string(key);
      }

      auto it = as_attrs(subject).attrs.find(name);
      if (it == as_attrs(subject).attrs.end()) {
        return value::make_bool(false);
      }
      subject = force(it->second);
    }
    return value::make_bool(true);
  }

  auto eval_variant(const ast::expression_lambda& expr, env_ptr env) -> value_ptr {
    return value::make_closure(&expr, env);
  }

  auto eval_variant(const ast::expression_application& expr, env_ptr env) -> value_ptr {
    auto func = eval_expr(expr.function, env);
    value_ptr result = func;
    for (const auto& arg : expr.arguments) {
      auto arg_thunk = value::make_thunk(arg, env);
      result = apply(result, arg_thunk);
    }
    return result;
  }

  auto apply(value_ptr func, value_ptr arg) -> value_ptr {
    func = force(func);

    if (is_closure(func)) {
      auto& c = as_closure(func);
      auto new_env = std::make_shared<environment>(c.env);

      auto& pattern = *c.lambda->argument_pattern;
      if (auto* simple = std::get_if<ast::pattern_simple>(&pattern)) {
        new_env->bind(symbols_.lookup(simple->argument_name), arg);
      } else if (auto* attrs_pat = std::get_if<ast::pattern_attrset>(&pattern)) {
        auto arg_val = force(arg);
        if (!is_attrs(arg_val))
          throw eval_error("function expects attrset argument");

        // Bind @name if present
        if (attrs_pat->argument_name) {
          new_env->bind(symbols_.lookup(*attrs_pat->argument_name), arg_val);
        }

        // Bind formals
        for (const auto& formal : attrs_pat->formals) {
          std::string name(symbols_.lookup(formal.name));
          auto it = as_attrs(arg_val).attrs.find(name);
          if (it != as_attrs(arg_val).attrs.end()) {
            new_env->bind(name, it->second);
          } else if (formal.default_value) {
            new_env->bind(name, value::make_thunk(*formal.default_value, new_env));
          } else {
            throw eval_error("missing required argument: " + name);
          }
        }
      }

      return eval_expr(c.lambda->body, new_env);
    }

    if (is_builtin(func)) {
      // Copy the builtin to avoid mutating the original
      builtin b = as_builtin(func);
      b.applied_args.push_back(arg);
      if (b.applied_args.size() >= b.arity) {
        return b.func(b.applied_args);
      }
      // Partial application - return new builtin with args accumulated
      return std::make_shared<value>(std::move(b));
    }

    throw eval_error("cannot apply non-function");
  }

  auto eval_variant(const ast::expression_let& expr, env_ptr env) -> value_ptr {
    auto let_env = std::make_shared<environment>(env);

    // First pass: create thunks for all bindings
    for (const auto& binding_var : expr.bindings) {
      if (auto* b = std::get_if<ast::binding_attribute>(&binding_var)) {
        if (b->path.segments.size() == 1) {
          auto& seg = b->path.segments[0];
          if (auto* sym = std::get_if<ast::symbol>(&seg.value)) {
            auto name = symbols_.lookup(*sym);
            let_env->bind(name, value::make_thunk(b->value, let_env));
          }
        }
      } else if (auto* inh = std::get_if<ast::binding_inherit>(&binding_var)) {
        if (inh->from_expression) {
          auto src = force(eval_expr(*inh->from_expression, env));
          if (!is_attrs(src))
            throw eval_error("inherit source must be attrset");
          for (const auto& attr : inh->attributes) {
            auto* sym = std::get_if<ast::symbol>(&attr.value);
            if (!sym)
              throw eval_error("dynamic attribute in inherit");
            std::string name(symbols_.lookup(*sym));
            auto it = as_attrs(src).attrs.find(name);
            if (it == as_attrs(src).attrs.end()) {
              throw eval_error("inherited attribute '" + name + "' not found");
            }
            let_env->bind(name, it->second);
          }
        } else {
          for (const auto& attr : inh->attributes) {
            auto* sym = std::get_if<ast::symbol>(&attr.value);
            if (!sym)
              throw eval_error("dynamic attribute in inherit");
            std::string name(symbols_.lookup(*sym));
            auto val = env->lookup(name);
            if (!val)
              throw eval_error("inherited variable '" + name + "' not found");
            let_env->bind(name, val);
          }
        }
      }
    }

    return eval_expr(expr.body, let_env);
  }

  auto eval_variant(const ast::expression_with& expr, env_ptr env) -> value_ptr {
    auto ns = force(eval_expr(expr.namespace_expression, env));
    if (!is_attrs(ns))
      throw eval_error("with: expected attrset");

    auto with_env = std::make_shared<environment>(env);
    for (const auto& [name, val] : as_attrs(ns).attrs) {
      with_env->bind(name, val);
    }

    return eval_expr(expr.body, with_env);
  }

  auto eval_variant(const ast::expression_if& expr, env_ptr env) -> value_ptr {
    auto cond = force(eval_expr(expr.condition, env));
    if (!is_bool(cond))
      throw eval_error("if condition must be boolean");

    if (as_bool(cond)) {
      return eval_expr(expr.then_branch, env);
    } else {
      return eval_expr(expr.else_branch, env);
    }
  }

  auto eval_variant(const ast::expression_assert& expr, env_ptr env) -> value_ptr {
    auto cond = force(eval_expr(expr.condition, env));
    if (!is_bool(cond))
      throw eval_error("assert condition must be boolean");
    if (!as_bool(cond))
      throw eval_error("assertion failed");
    return eval_expr(expr.body, env);
  }

  auto eval_variant(const ast::expression_binary_operation& expr, env_ptr env) -> value_ptr {
    // Short-circuit operators
    if (expr.op == ast::binary_operator::logical_and) {
      auto left = force(eval_expr(expr.left, env));
      if (!is_bool(left))
        throw eval_error("&& requires boolean");
      if (!as_bool(left))
        return value::make_bool(false);
      return eval_expr(expr.right, env);
    }

    if (expr.op == ast::binary_operator::logical_or) {
      auto left = force(eval_expr(expr.left, env));
      if (!is_bool(left))
        throw eval_error("|| requires boolean");
      if (as_bool(left))
        return value::make_bool(true);
      return eval_expr(expr.right, env);
    }

    if (expr.op == ast::binary_operator::logical_implies) {
      auto left = force(eval_expr(expr.left, env));
      if (!is_bool(left))
        throw eval_error("-> requires boolean");
      if (!as_bool(left))
        return value::make_bool(true);
      return eval_expr(expr.right, env);
    }

    auto left = force(eval_expr(expr.left, env));
    auto right = force(eval_expr(expr.right, env));

    switch (expr.op) {
      case ast::binary_operator::add:
        if (is_int(left) && is_int(right)) {
          return value::make_int(as_int(left) + as_int(right));
        }
        if ((is_int(left) || is_float(left)) && (is_int(right) || is_float(right))) {
          double l = is_int(left) ? static_cast<double>(as_int(left)) : as_float(left);
          double r = is_int(right) ? static_cast<double>(as_int(right)) : as_float(right);
          return value::make_float(l + r);
        }
        if (is_string(left) && is_string(right)) {
          return value::make_string(as_string(left) + as_string(right));
        }
        if (is_path(left) && is_string(right)) {
          return value::make_path(as_path(left).path + "/" + as_string(right));
        }
        throw eval_error("cannot add these values");

      case ast::binary_operator::subtract:
        if (is_int(left) && is_int(right)) {
          return value::make_int(as_int(left) - as_int(right));
        }
        throw eval_error("cannot subtract these values");

      case ast::binary_operator::multiply:
        if (is_int(left) && is_int(right)) {
          return value::make_int(as_int(left) * as_int(right));
        }
        throw eval_error("cannot multiply these values");

      case ast::binary_operator::divide:
        if (is_int(left) && is_int(right)) {
          if (as_int(right) == 0)
            throw eval_error("division by zero");
          return value::make_int(as_int(left) / as_int(right));
        }
        throw eval_error("cannot divide these values");

      case ast::binary_operator::equals:
        return value::make_bool(values_equal(left, right));

      case ast::binary_operator::not_equals:
        return value::make_bool(!values_equal(left, right));

      case ast::binary_operator::less_than:
        if (is_int(left) && is_int(right)) {
          return value::make_bool(as_int(left) < as_int(right));
        }
        throw eval_error("cannot compare these values");

      case ast::binary_operator::less_than_or_equal:
        if (is_int(left) && is_int(right)) {
          return value::make_bool(as_int(left) <= as_int(right));
        }
        throw eval_error("cannot compare these values");

      case ast::binary_operator::greater_than:
        if (is_int(left) && is_int(right)) {
          return value::make_bool(as_int(left) > as_int(right));
        }
        throw eval_error("cannot compare these values");

      case ast::binary_operator::greater_than_or_equal:
        if (is_int(left) && is_int(right)) {
          return value::make_bool(as_int(left) >= as_int(right));
        }
        throw eval_error("cannot compare these values");

      case ast::binary_operator::concatenate:
        if (is_list(left) && is_list(right)) {
          auto& l = as_list(left);
          auto& r = as_list(right);
          std::vector<value_ptr> result;
          result.reserve(l.elements.size() + r.elements.size());
          result.insert(result.end(), l.elements.begin(), l.elements.end());
          result.insert(result.end(), r.elements.begin(), r.elements.end());
          return value::make_list(std::move(result));
        }
        throw eval_error("can only concatenate lists");

      case ast::binary_operator::update:
        if (is_attrs(left) && is_attrs(right)) {
          std::unordered_map<std::string, value_ptr> result = as_attrs(left).attrs;
          for (const auto& [k, v] : as_attrs(right).attrs) {
            result[k] = v;
          }
          return value::make_attrs(std::move(result));
        }
        throw eval_error("can only update attrsets");

      default:
        throw eval_error("unimplemented binary operator");
    }
  }

  auto eval_variant(const ast::expression_unary_operation& expr, env_ptr env) -> value_ptr {
    auto operand = force(eval_expr(expr.operand, env));

    switch (expr.op) {
      case ast::unary_operator::negate:
        if (is_int(operand))
          return value::make_int(-as_int(operand));
        if (is_float(operand))
          return value::make_float(-as_float(operand));
        throw eval_error("cannot negate non-numeric value");

      case ast::unary_operator::logical_not:
        if (is_bool(operand))
          return value::make_bool(!as_bool(operand));
        throw eval_error("cannot negate non-boolean value");

      default:
        throw eval_error("unimplemented unary operator");
    }
  }

  auto values_equal(const value_ptr& a, const value_ptr& b) -> bool {
    auto va = force(a);
    auto vb = force(b);

    if (is_bool(va) && is_bool(vb))
      return as_bool(va) == as_bool(vb);
    if (is_int(va) && is_int(vb))
      return as_int(va) == as_int(vb);
    if (is_float(va) && is_float(vb))
      return as_float(va) == as_float(vb);
    if (is_string(va) && is_string(vb))
      return as_string(va) == as_string(vb);
    if (is_path(va) && is_path(vb))
      return as_path(va).path == as_path(vb).path;
    if (is_list(va) && is_list(vb)) {
      auto& la = as_list(va);
      auto& lb = as_list(vb);
      if (la.elements.size() != lb.elements.size())
        return false;
      for (std::size_t i = 0; i < la.elements.size(); ++i) {
        if (!values_equal(la.elements[i], lb.elements[i]))
          return false;
      }
      return true;
    }
    if (std::holds_alternative<value_null>(va->data) &&
        std::holds_alternative<value_null>(vb->data))
      return true;

    return false;
  }
};

} // namespace nix::language::eval
