#pragma once
/// @file nix-language/eval/eval.hh
/// Tree-walking interpreter for Nix expressions.

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <unordered_set>

#include "nix-language/ast/expression.hh"
#include "nix-language/ast/symbol_table.hh"
#include "nix-language/eval/value.hh"

namespace nix::language::eval {

/// Evaluation error
class eval_error : public std::runtime_error {
public:
  using std::runtime_error::runtime_error;
};

/// Function type for parsing a file - returns an expression
/// This is used to break the circular dependency between eval and parse
using file_parser =
    std::function<ast::expression(const std::string& path, const std::string& source)>;

/// The interpreter
class evaluator {
public:
  explicit evaluator(ast::symbol_table& symbols) : symbols_(symbols) {
    root_env_ = std::make_shared<environment>();
    setup_builtins();
  }

  /// Set the file parser for import support
  void set_file_parser(file_parser parser) { file_parser_ = std::move(parser); }

  /// Set the base path for resolving relative imports
  void set_base_path(const std::filesystem::path& path) { base_path_ = path; }

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

  /// Recursively force all nested values
  void deep_force(value_ptr val) {
    val = force(val);
    if (is_list(val)) {
      for (const auto& elem : as_list(val).elements) {
        deep_force(elem);
      }
    } else if (is_attrs(val)) {
      for (const auto& [k, v] : as_attrs(val).attrs) {
        deep_force(v);
      }
    }
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
  file_parser file_parser_;
  std::filesystem::path base_path_ = std::filesystem::current_path();
  std::unordered_set<std::string> imported_files_;    // Track imported files to detect cycles
  std::vector<ast::expression> imported_expressions_; // Keep parsed expressions alive

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

    // builtins.elem - check if element is in list
    builtins_map["elem"] = value::make_builtin("elem", 2, [this](std::vector<value_ptr>& args) {
      auto needle = force(args[0]);
      auto lst = force(args[1]);
      if (!is_list(lst))
        throw eval_error("elem: second arg must be list");
      for (const auto& elem : as_list(lst).elements) {
        if (values_equal(needle, force(elem)))
          return value::make_bool(true);
      }
      return value::make_bool(false);
    });

    // builtins.all - check if predicate holds for all elements
    builtins_map["all"] = value::make_builtin("all", 2, [this](std::vector<value_ptr>& args) {
      auto pred = args[0];
      auto lst = force(args[1]);
      if (!is_list(lst))
        throw eval_error("all: second arg must be list");
      for (const auto& elem : as_list(lst).elements) {
        auto result = force(apply(pred, elem));
        if (!is_bool(result))
          throw eval_error("all: predicate must return bool");
        if (!as_bool(result))
          return value::make_bool(false);
      }
      return value::make_bool(true);
    });

    // builtins.any - check if predicate holds for any element
    builtins_map["any"] = value::make_builtin("any", 2, [this](std::vector<value_ptr>& args) {
      auto pred = args[0];
      auto lst = force(args[1]);
      if (!is_list(lst))
        throw eval_error("any: second arg must be list");
      for (const auto& elem : as_list(lst).elements) {
        auto result = force(apply(pred, elem));
        if (!is_bool(result))
          throw eval_error("any: predicate must return bool");
        if (as_bool(result))
          return value::make_bool(true);
      }
      return value::make_bool(false);
    });

    // builtins.concatLists - concatenate a list of lists
    builtins_map["concatLists"] =
        value::make_builtin("concatLists", 1, [this](std::vector<value_ptr>& args) {
          auto lists = force(args[0]);
          if (!is_list(lists))
            throw eval_error("concatLists: expected list of lists");
          std::vector<value_ptr> result;
          for (const auto& lst : as_list(lists).elements) {
            auto l = force(lst);
            if (!is_list(l))
              throw eval_error("concatLists: element is not a list");
            for (const auto& elem : as_list(l).elements) {
              result.push_back(elem);
            }
          }
          return value::make_list(std::move(result));
        });

    // builtins.concatMap - map then concat
    builtins_map["concatMap"] =
        value::make_builtin("concatMap", 2, [this](std::vector<value_ptr>& args) {
          auto func = args[0];
          auto lst = force(args[1]);
          if (!is_list(lst))
            throw eval_error("concatMap: second arg must be list");
          std::vector<value_ptr> result;
          for (const auto& elem : as_list(lst).elements) {
            auto mapped = force(apply(func, elem));
            if (!is_list(mapped))
              throw eval_error("concatMap: function must return list");
            for (const auto& item : as_list(mapped).elements) {
              result.push_back(item);
            }
          }
          return value::make_list(std::move(result));
        });

    // builtins.isNull
    builtins_map["isNull"] = value::make_builtin("isNull", 1, [this](std::vector<value_ptr>& args) {
      auto v = force(args[0]);
      return value::make_bool(is_null(v));
    });

    // builtins.isBool
    builtins_map["isBool"] = value::make_builtin("isBool", 1, [this](std::vector<value_ptr>& args) {
      auto v = force(args[0]);
      return value::make_bool(is_bool(v));
    });

    // builtins.isInt
    builtins_map["isInt"] = value::make_builtin("isInt", 1, [this](std::vector<value_ptr>& args) {
      auto v = force(args[0]);
      return value::make_bool(is_int(v));
    });

    // builtins.isFloat
    builtins_map["isFloat"] =
        value::make_builtin("isFloat", 1, [this](std::vector<value_ptr>& args) {
          auto v = force(args[0]);
          return value::make_bool(is_float(v));
        });

    // builtins.isString
    builtins_map["isString"] =
        value::make_builtin("isString", 1, [this](std::vector<value_ptr>& args) {
          auto v = force(args[0]);
          return value::make_bool(is_string(v));
        });

    // builtins.isList
    builtins_map["isList"] = value::make_builtin("isList", 1, [this](std::vector<value_ptr>& args) {
      auto v = force(args[0]);
      return value::make_bool(is_list(v));
    });

    // builtins.isAttrs
    builtins_map["isAttrs"] =
        value::make_builtin("isAttrs", 1, [this](std::vector<value_ptr>& args) {
          auto v = force(args[0]);
          return value::make_bool(is_attrs(v));
        });

    // builtins.isFunction
    builtins_map["isFunction"] =
        value::make_builtin("isFunction", 1, [this](std::vector<value_ptr>& args) {
          auto v = force(args[0]);
          return value::make_bool(is_closure(v) || is_builtin(v));
        });

    // builtins.isPath
    builtins_map["isPath"] = value::make_builtin("isPath", 1, [this](std::vector<value_ptr>& args) {
      auto v = force(args[0]);
      return value::make_bool(is_path(v));
    });

    // builtins.attrValues - get list of values from attrset
    builtins_map["attrValues"] =
        value::make_builtin("attrValues", 1, [this](std::vector<value_ptr>& args) {
          auto attrs = force(args[0]);
          if (!is_attrs(attrs))
            throw eval_error("attrValues: expected attrset");
          std::vector<value_ptr> values;
          for (const auto& [k, v] : as_attrs(attrs).attrs) {
            values.push_back(v);
          }
          return value::make_list(std::move(values));
        });

    // builtins.listToAttrs - convert list of {name, value} to attrset
    builtins_map["listToAttrs"] =
        value::make_builtin("listToAttrs", 1, [this](std::vector<value_ptr>& args) {
          auto lst = force(args[0]);
          if (!is_list(lst))
            throw eval_error("listToAttrs: expected list");
          std::unordered_map<std::string, value_ptr> result;
          for (const auto& elem : as_list(lst).elements) {
            auto e = force(elem);
            if (!is_attrs(e))
              throw eval_error("listToAttrs: element must be attrset");
            const auto& attrs = as_attrs(e).attrs;
            auto name_it = attrs.find("name");
            auto value_it = attrs.find("value");
            if (name_it == attrs.end() || value_it == attrs.end())
              throw eval_error("listToAttrs: element must have 'name' and 'value'");
            auto name = force(name_it->second);
            if (!is_string(name))
              throw eval_error("listToAttrs: 'name' must be string");
            // First occurrence wins (Nix behavior)
            if (result.find(as_string(name)) == result.end()) {
              result[as_string(name)] = value_it->second;
            }
          }
          return value::make_attrs(std::move(result));
        });

    // builtins.mapAttrs - map over attrset values
    builtins_map["mapAttrs"] =
        value::make_builtin("mapAttrs", 2, [this](std::vector<value_ptr>& args) {
          auto func = args[0];
          auto attrs = force(args[1]);
          if (!is_attrs(attrs))
            throw eval_error("mapAttrs: second arg must be attrset");
          std::unordered_map<std::string, value_ptr> result;
          for (const auto& [k, v] : as_attrs(attrs).attrs) {
            auto f1 = apply(func, value::make_string(k));
            result[k] = apply(f1, v);
          }
          return value::make_attrs(std::move(result));
        });

    // builtins.genList - generate list from function
    builtins_map["genList"] =
        value::make_builtin("genList", 2, [this](std::vector<value_ptr>& args) {
          auto func = args[0];
          auto len = force(args[1]);
          if (!is_int(len))
            throw eval_error("genList: second arg must be int");
          auto n = as_int(len);
          if (n < 0)
            throw eval_error("genList: length must be non-negative");
          std::vector<value_ptr> result;
          result.reserve(static_cast<std::size_t>(n));
          for (std::int64_t i = 0; i < n; ++i) {
            result.push_back(apply(func, value::make_int(i)));
          }
          return value::make_list(std::move(result));
        });

    // builtins.sort - sort a list
    builtins_map["sort"] = value::make_builtin("sort", 2, [this](std::vector<value_ptr>& args) {
      auto comparator = args[0];
      auto lst = force(args[1]);
      if (!is_list(lst))
        throw eval_error("sort: second arg must be list");
      std::vector<value_ptr> result = as_list(lst).elements;
      std::sort(result.begin(), result.end(),
                [this, &comparator](const value_ptr& a, const value_ptr& b) {
                  auto f1 = apply(comparator, a);
                  auto cmp = force(apply(f1, b));
                  if (!is_bool(cmp))
                    throw eval_error("sort: comparator must return bool");
                  return as_bool(cmp);
                });
      return value::make_list(std::move(result));
    });

    // builtins.elemAt - get element at index
    builtins_map["elemAt"] = value::make_builtin("elemAt", 2, [this](std::vector<value_ptr>& args) {
      auto lst = force(args[0]);
      auto idx = force(args[1]);
      if (!is_list(lst))
        throw eval_error("elemAt: first arg must be list");
      if (!is_int(idx))
        throw eval_error("elemAt: second arg must be int");
      auto i = as_int(idx);
      const auto& elements = as_list(lst).elements;
      if (i < 0 || static_cast<std::size_t>(i) >= elements.size())
        throw eval_error("elemAt: index out of bounds");
      return elements[static_cast<std::size_t>(i)];
    });

    // builtins.stringLength
    builtins_map["stringLength"] =
        value::make_builtin("stringLength", 1, [this](std::vector<value_ptr>& args) {
          auto s = force(args[0]);
          if (!is_string(s))
            throw eval_error("stringLength: expected string");
          return value::make_int(static_cast<std::int64_t>(as_string(s).size()));
        });

    // builtins.substring
    builtins_map["substring"] =
        value::make_builtin("substring", 3, [this](std::vector<value_ptr>& args) {
          auto start = force(args[0]);
          auto len = force(args[1]);
          auto s = force(args[2]);
          if (!is_int(start))
            throw eval_error("substring: first arg must be int");
          if (!is_int(len))
            throw eval_error("substring: second arg must be int");
          if (!is_string(s))
            throw eval_error("substring: third arg must be string");
          auto st = as_int(start);
          auto ln = as_int(len);
          const auto& str = as_string(s);
          if (st < 0)
            st = 0;
          if (static_cast<std::size_t>(st) >= str.size())
            return value::make_string("");
          return value::make_string(
              str.substr(static_cast<std::size_t>(st), static_cast<std::size_t>(ln)));
        });

    // builtins.replaceStrings
    builtins_map["replaceStrings"] =
        value::make_builtin("replaceStrings", 3, [this](std::vector<value_ptr>& args) {
          auto from_list = force(args[0]);
          auto to_list = force(args[1]);
          auto s = force(args[2]);
          if (!is_list(from_list) || !is_list(to_list))
            throw eval_error("replaceStrings: first two args must be lists");
          if (!is_string(s))
            throw eval_error("replaceStrings: third arg must be string");
          const auto& froms = as_list(from_list).elements;
          const auto& tos = as_list(to_list).elements;
          if (froms.size() != tos.size())
            throw eval_error("replaceStrings: lists must have same length");

          std::vector<std::string> from_strs, to_strs;
          for (std::size_t i = 0; i < froms.size(); ++i) {
            auto f = force(froms[i]);
            auto t = force(tos[i]);
            if (!is_string(f) || !is_string(t))
              throw eval_error("replaceStrings: list elements must be strings");
            from_strs.push_back(as_string(f));
            to_strs.push_back(as_string(t));
          }

          std::string result;
          std::string input = as_string(s);
          std::size_t pos = 0;
          while (pos < input.size()) {
            bool replaced = false;
            for (std::size_t i = 0; i < from_strs.size(); ++i) {
              if (from_strs[i].empty()) {
                // Empty string matches at current position
                result += to_strs[i];
                if (pos < input.size()) {
                  result += input[pos];
                  ++pos;
                }
                replaced = true;
                break;
              }
              if (input.compare(pos, from_strs[i].size(), from_strs[i]) == 0) {
                result += to_strs[i];
                pos += from_strs[i].size();
                replaced = true;
                break;
              }
            }
            if (!replaced) {
              result += input[pos];
              ++pos;
            }
          }
          // Handle empty string match at end
          for (std::size_t i = 0; i < from_strs.size(); ++i) {
            if (from_strs[i].empty()) {
              result += to_strs[i];
              break;
            }
          }
          return value::make_string(result);
        });

    // builtins.seq - force first arg, return second
    builtins_map["seq"] = value::make_builtin("seq", 2, [this](std::vector<value_ptr>& args) {
      force(args[0]);
      return args[1];
    });

    // builtins.deepSeq - recursively force first arg, return second
    builtins_map["deepSeq"] =
        value::make_builtin("deepSeq", 2, [this](std::vector<value_ptr>& args) {
          deep_force(args[0]);
          return args[1];
        });

    // builtins.tryEval - try to evaluate, return { success, value }
    builtins_map["tryEval"] =
        value::make_builtin("tryEval", 1, [this](std::vector<value_ptr>& args) {
          std::unordered_map<std::string, value_ptr> result;
          try {
            auto v = force(args[0]);
            result["success"] = value::make_bool(true);
            result["value"] = v;
          } catch (const eval_error&) {
            result["success"] = value::make_bool(false);
            result["value"] = value::make_bool(false);
          }
          return value::make_attrs(std::move(result));
        });

    // builtins.trace - print message and return value
    builtins_map["trace"] = value::make_builtin("trace", 2, [this](std::vector<value_ptr>& args) {
      auto msg = force(args[0]);
      std::cerr << "trace: " << print_value(msg) << std::endl;
      return args[1];
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

    // builtins.import - load and evaluate a Nix file
    builtins_map["import"] =
        value::make_builtin("import", 1, [this](std::vector<value_ptr>& args) -> value_ptr {
          if (!file_parser_) {
            throw eval_error("import: file parser not configured");
          }

          auto path_val = force(args[0]);
          std::filesystem::path file_path;

          if (is_path(path_val)) {
            file_path = as_path(path_val).path;
          } else if (is_string(path_val)) {
            file_path = as_string(path_val);
          } else {
            throw eval_error("import: expected path or string");
          }

          // Resolve relative paths
          if (file_path.is_relative()) {
            file_path = base_path_ / file_path;
          }

          // Normalize the path
          file_path = std::filesystem::weakly_canonical(file_path);

          // If it's a directory, look for default.nix
          if (std::filesystem::is_directory(file_path)) {
            file_path = file_path / "default.nix";
          }

          std::string path_str = file_path.string();

          // Check for import cycles
          if (imported_files_.count(path_str) > 0) {
            throw eval_error("import: cycle detected importing '" + path_str + "'");
          }

          // Read the file
          std::ifstream file(file_path);
          if (!file) {
            throw eval_error("import: cannot open file '" + path_str + "'");
          }
          std::stringstream buffer;
          buffer << file.rdbuf();
          std::string source = buffer.str();

          // Track this import
          imported_files_.insert(path_str);

          // Save and set the base path for nested imports
          auto old_base = base_path_;
          base_path_ = file_path.parent_path();

          try {
            // Parse and store the expression (must keep it alive for thunks)
            imported_expressions_.push_back(file_parser_(path_str, source));
            const ast::expression& expr = imported_expressions_.back();
            auto result = eval_expr(expr, root_env_);

            // Restore state
            base_path_ = old_base;
            imported_files_.erase(path_str);

            return result;
          } catch (...) {
            // Restore state on error
            base_path_ = old_base;
            imported_files_.erase(path_str);
            throw;
          }
        });

    // builtins.readFile - read file contents as string
    builtins_map["readFile"] =
        value::make_builtin("readFile", 1, [this](std::vector<value_ptr>& args) -> value_ptr {
          auto path_val = force(args[0]);
          std::filesystem::path file_path;

          if (is_path(path_val)) {
            file_path = as_path(path_val).path;
          } else if (is_string(path_val)) {
            file_path = as_string(path_val);
          } else {
            throw eval_error("readFile: expected path or string");
          }

          // Resolve relative paths
          if (file_path.is_relative()) {
            file_path = base_path_ / file_path;
          }

          std::ifstream file(file_path);
          if (!file) {
            throw eval_error("readFile: cannot open file '" + file_path.string() + "'");
          }
          std::stringstream buffer;
          buffer << file.rdbuf();
          return value::make_string(buffer.str());
        });

    // builtins.pathExists - check if path exists
    builtins_map["pathExists"] =
        value::make_builtin("pathExists", 1, [this](std::vector<value_ptr>& args) -> value_ptr {
          auto path_val = force(args[0]);
          std::filesystem::path file_path;

          if (is_path(path_val)) {
            file_path = as_path(path_val).path;
          } else if (is_string(path_val)) {
            file_path = as_string(path_val);
          } else {
            throw eval_error("pathExists: expected path or string");
          }

          // Resolve relative paths
          if (file_path.is_relative()) {
            file_path = base_path_ / file_path;
          }

          return value::make_bool(std::filesystem::exists(file_path));
        });

    // builtins.baseNameOf - get filename from path
    builtins_map["baseNameOf"] =
        value::make_builtin("baseNameOf", 1, [this](std::vector<value_ptr>& args) -> value_ptr {
          auto path_val = force(args[0]);
          std::string path_str;

          if (is_path(path_val)) {
            path_str = as_path(path_val).path;
          } else if (is_string(path_val)) {
            path_str = as_string(path_val);
          } else {
            throw eval_error("baseNameOf: expected path or string");
          }

          std::filesystem::path p(path_str);
          return value::make_string(p.filename().string());
        });

    // builtins.dirOf - get directory from path
    builtins_map["dirOf"] =
        value::make_builtin("dirOf", 1, [this](std::vector<value_ptr>& args) -> value_ptr {
          auto path_val = force(args[0]);
          std::string path_str;

          if (is_path(path_val)) {
            path_str = as_path(path_val).path;
          } else if (is_string(path_val)) {
            path_str = as_string(path_val);
          } else {
            throw eval_error("dirOf: expected path or string");
          }

          std::filesystem::path p(path_str);
          return value::make_string(p.parent_path().string());
        });

    // builtins.null constant
    builtins_map["null"] = value::make_null();

    // builtins.true and builtins.false
    builtins_map["true"] = value::make_bool(true);
    builtins_map["false"] = value::make_bool(false);

    // Bind import at top level before moving builtins_map
    // import is a global builtin in Nix (not just builtins.import)
    root_env_->bind("import", builtins_map["import"]);

    // Also bind other commonly-used builtins at top level
    root_env_->bind("baseNameOf", builtins_map["baseNameOf"]);
    root_env_->bind("dirOf", builtins_map["dirOf"]);
    root_env_->bind("toString", builtins_map["toString"]);
    root_env_->bind("throw", builtins_map["throw"]);
    root_env_->bind("abort", builtins_map["abort"]);
    root_env_->bind("map", builtins_map["map"]);

    root_env_->bind("builtins", value::make_attrs(std::move(builtins_map)));

    // Also bind common builtins at top level for convenience
    root_env_->bind("null", value::make_null());
    root_env_->bind("true", value::make_bool(true));
    root_env_->bind("false", value::make_bool(false));
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
