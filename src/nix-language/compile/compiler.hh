#pragma once
///@file nix-language/compile/compiler.hh
/// Compiles Nix AST to WebAssembly using binaryen.

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <binaryen-c.h>

#include "nix-language/ast/expression.hh"
#include "nix-language/ast/symbol_table.hh"
#include "nix-language/compile/wasm_types.hh"

namespace nix::language::compile {

/// hash function for ast::symbol to use in unordered containers
struct symbol_hash {
  auto operator()(ast::symbol s) const noexcept -> std::size_t {
    return std::hash<std::uint32_t>{}(s.index);
  }
};

/// compilation error
class compilation_error : public std::runtime_error {
public:
  using std::runtime_error::runtime_error;
};

/// where a variable is stored at runtime
enum class variable_location {
  local,    // in a WASM local variable
  captured, // in the closure environment
};

/// represents a variable binding in a lexical scope
struct variable_binding {
  ast::symbol name;
  std::uint32_t local_index;   // WASM local index (if location == local)
  std::uint32_t capture_index; // index in closure captures (if location == captured)
  variable_location location;  // where the variable is stored
  std::uint32_t scope_depth;   // depth of scope where defined (0 = current function)
  bool is_captured_by_inner;   // true if captured by an inner lambda
};

/// lexical scope for variable resolution during compilation
class lexical_scope {
public:
  explicit lexical_scope(lexical_scope* parent = nullptr, std::uint32_t depth = 0)
      : parent_(parent), depth_(depth) {}

  /// add a local variable to the current scope
  auto add_local(ast::symbol name, std::uint32_t local_index) -> std::uint32_t {
    variables_.push_back({name, local_index, 0, variable_location::local, depth_, false});
    return local_index;
  }

  /// add a captured variable to the current scope
  auto add_captured(ast::symbol name, std::uint32_t capture_index) -> std::uint32_t {
    variables_.push_back({name, 0, capture_index, variable_location::captured, depth_, false});
    return capture_index;
  }

  /// look up a variable by name in current scope only
  [[nodiscard]] auto lookup_local(ast::symbol name) const -> std::optional<variable_binding> {
    for (const auto& var : variables_) {
      if (var.name == name) {
        return var;
      }
    }
    return std::nullopt;
  }

  /// look up a variable by name, searching parent scopes
  /// returns the binding and the depth difference (0 = current scope)
  [[nodiscard]] auto lookup(ast::symbol name) const
      -> std::optional<std::pair<variable_binding, std::uint32_t>> {
    // search current scope
    for (const auto& var : variables_) {
      if (var.name == name) {
        return std::make_pair(var, 0u);
      }
    }
    // search parent scopes
    if (parent_) {
      auto result = parent_->lookup(name);
      if (result.has_value()) {
        result->second += 1; // increment depth
        return result;
      }
    }
    return std::nullopt;
  }

  /// mark a variable as captured by an inner lambda
  void mark_captured(ast::symbol name) {
    for (auto& var : variables_) {
      if (var.name == name) {
        var.is_captured_by_inner = true;
        return;
      }
    }
    if (parent_) {
      parent_->mark_captured(name);
    }
  }

  /// get the parent scope
  [[nodiscard]] auto parent() const noexcept -> lexical_scope* { return parent_; }

  /// get the depth of this scope
  [[nodiscard]] auto depth() const noexcept -> std::uint32_t { return depth_; }

  /// get all variables in this scope (not including parents)
  [[nodiscard]] auto variables() const noexcept -> const std::vector<variable_binding>& {
    return variables_;
  }

private:
  lexical_scope* parent_;
  std::uint32_t depth_;
  std::vector<variable_binding> variables_;
};

/// finds free variables in an expression
/// free variables are identifiers referenced but not bound within the expression
class free_variable_analyzer {
public:
  /// analyze an expression and return its free variables
  /// bound_names: names already bound in the enclosing scope
  [[nodiscard]] static auto analyze(const ast::expression& expr,
                                    const std::vector<ast::symbol>& bound_names)
      -> std::vector<ast::symbol> {
    free_variable_analyzer analyzer;
    for (auto name : bound_names) {
      analyzer.bound_.insert(name);
    }
    analyzer.visit(expr);
    return std::vector<ast::symbol>(analyzer.free_.begin(), analyzer.free_.end());
  }

private:
  std::unordered_set<ast::symbol, symbol_hash> bound_;
  std::unordered_set<ast::symbol, symbol_hash> free_;

  void visit(const ast::expression& expr) {
    std::visit([this](const auto& e) { visit_variant(e); }, expr->data);
  }

  void visit_variant(const ast::expression_identifier& expr) {
    if (bound_.find(expr.name) == bound_.end()) {
      free_.insert(expr.name);
    }
  }

  void visit_variant(const ast::expression_integer&) {}
  void visit_variant(const ast::expression_float&) {}
  void visit_variant(const ast::expression_string&) {}
  void visit_variant(const ast::expression_path&) {}

  void visit_variant(const ast::expression_string_interpolated& expr) {
    for (const auto& part : expr.parts) {
      if (std::holds_alternative<ast::expression>(part)) {
        visit(std::get<ast::expression>(part));
      }
    }
  }

  void visit_variant(const ast::expression_path_interpolated& expr) {
    for (const auto& part : expr.parts) {
      if (std::holds_alternative<ast::expression>(part)) {
        visit(std::get<ast::expression>(part));
      }
    }
  }

  void visit_variant(const ast::expression_binary_operation& expr) {
    visit(expr.left);
    visit(expr.right);
  }

  void visit_variant(const ast::expression_unary_operation& expr) { visit(expr.operand); }

  void visit_variant(const ast::expression_list& expr) {
    for (const auto& element : expr.elements) {
      visit(element);
    }
  }

  void visit_variant(const ast::expression_attribute_set& expr) {
    // for rec sets, all binding names are in scope for all values
    std::vector<ast::symbol> new_bindings;
    if (expr.is_recursive) {
      for (const auto& binding : expr.bindings) {
        if (std::holds_alternative<ast::binding_attribute>(binding)) {
          const auto& attr_binding = std::get<ast::binding_attribute>(binding);
          if (attr_binding.path.segments.size() == 1 &&
              !attr_binding.path.segments[0].is_dynamic()) {
            auto sym = std::get<ast::symbol>(attr_binding.path.segments[0].value);
            new_bindings.push_back(sym);
            bound_.insert(sym);
          }
        }
      }
    }

    for (const auto& binding : expr.bindings) {
      if (std::holds_alternative<ast::binding_attribute>(binding)) {
        const auto& attr_binding = std::get<ast::binding_attribute>(binding);
        visit(attr_binding.value);
        // visit dynamic path segments
        for (const auto& segment : attr_binding.path.segments) {
          if (segment.is_dynamic()) {
            visit(std::get<ast::expression>(segment.value));
          }
        }
      } else {
        const auto& inherit = std::get<ast::binding_inherit>(binding);
        if (inherit.from_expression.has_value()) {
          visit(*inherit.from_expression);
        } else {
          // inherit x; pulls x from outer scope
          for (const auto& attr : inherit.attributes) {
            if (!attr.is_dynamic() && std::holds_alternative<ast::symbol>(attr.value)) {
              auto name = std::get<ast::symbol>(attr.value);
              if (bound_.find(name) == bound_.end()) {
                free_.insert(name);
              }
            }
          }
        }
      }
    }

    // remove recursive bindings (restore scope)
    if (expr.is_recursive) {
      for (auto sym : new_bindings) {
        bound_.erase(sym);
      }
    }
  }

  void visit_variant(const ast::expression_select& expr) {
    visit(expr.subject);
    // visit dynamic path segments
    for (const auto& segment : expr.path.segments) {
      if (segment.is_dynamic()) {
        visit(std::get<ast::expression>(segment.value));
      }
    }
    if (expr.default_value.has_value()) {
      visit(*expr.default_value);
    }
  }

  void visit_variant(const ast::expression_has_attribute& expr) {
    visit(expr.subject);
    for (const auto& segment : expr.path.segments) {
      if (segment.is_dynamic()) {
        visit(std::get<ast::expression>(segment.value));
      }
    }
  }

  void visit_variant(const ast::expression_lambda& expr) {
    // lambda introduces new bindings
    std::vector<ast::symbol> lambda_bindings;

    const auto& pattern = *expr.argument_pattern;
    if (std::holds_alternative<ast::pattern_simple>(pattern)) {
      const auto& simple = std::get<ast::pattern_simple>(pattern);
      lambda_bindings.push_back(simple.argument_name);
    } else {
      const auto& attrset_pattern = std::get<ast::pattern_attrset>(pattern);
      if (attrset_pattern.argument_name.has_value()) {
        lambda_bindings.push_back(*attrset_pattern.argument_name);
      }
      for (const auto& formal : attrset_pattern.formals) {
        lambda_bindings.push_back(formal.name);
        // default values are evaluated in outer scope
        if (formal.default_value.has_value()) {
          visit(*formal.default_value);
        }
      }
    }

    // add lambda bindings to scope
    for (auto sym : lambda_bindings) {
      bound_.insert(sym);
    }

    // visit body
    visit(expr.body);

    // remove lambda bindings (restore scope)
    for (auto sym : lambda_bindings) {
      bound_.erase(sym);
    }
  }

  void visit_variant(const ast::expression_application& expr) {
    visit(expr.function);
    for (const auto& arg : expr.arguments) {
      visit(arg);
    }
  }

  void visit_variant(const ast::expression_let& expr) {
    // let bindings are mutually recursive
    std::vector<ast::symbol> let_bindings;
    for (const auto& binding : expr.bindings) {
      if (std::holds_alternative<ast::binding_attribute>(binding)) {
        const auto& attr_binding = std::get<ast::binding_attribute>(binding);
        if (attr_binding.path.segments.size() == 1 && !attr_binding.path.segments[0].is_dynamic()) {
          auto sym = std::get<ast::symbol>(attr_binding.path.segments[0].value);
          let_bindings.push_back(sym);
          bound_.insert(sym);
        }
      }
    }

    // visit binding values (in scope of all let bindings)
    for (const auto& binding : expr.bindings) {
      if (std::holds_alternative<ast::binding_attribute>(binding)) {
        const auto& attr_binding = std::get<ast::binding_attribute>(binding);
        visit(attr_binding.value);
      } else {
        const auto& inherit = std::get<ast::binding_inherit>(binding);
        if (inherit.from_expression.has_value()) {
          visit(*inherit.from_expression);
        } else {
          for (const auto& attr : inherit.attributes) {
            if (!attr.is_dynamic() && std::holds_alternative<ast::symbol>(attr.value)) {
              auto name = std::get<ast::symbol>(attr.value);
              if (bound_.find(name) == bound_.end()) {
                free_.insert(name);
              }
            }
          }
        }
      }
    }

    // visit body
    visit(expr.body);

    // restore scope
    for (auto sym : let_bindings) {
      bound_.erase(sym);
    }
  }

  void visit_variant(const ast::expression_with& expr) {
    visit(expr.namespace_expression);
    // with introduces dynamic scope, we can't statically know what's bound
    // conservatively, we visit the body without adding bindings
    // (actual free variable detection for `with` is conservative)
    visit(expr.body);
  }

  void visit_variant(const ast::expression_if& expr) {
    visit(expr.condition);
    visit(expr.then_branch);
    visit(expr.else_branch);
  }

  void visit_variant(const ast::expression_assert& expr) {
    visit(expr.condition);
    visit(expr.body);
  }
};

/// RAII wrapper for BinaryenModuleRef
class wasm_module {
public:
  wasm_module() : module_(BinaryenModuleCreate()) {}

  ~wasm_module() {
    if (module_) {
      BinaryenModuleDispose(module_);
    }
  }

  // non-copyable
  wasm_module(const wasm_module&) = delete;
  auto operator=(const wasm_module&) -> wasm_module& = delete;

  // movable
  wasm_module(wasm_module&& other) noexcept : module_(other.module_) { other.module_ = nullptr; }

  auto operator=(wasm_module&& other) noexcept -> wasm_module& {
    if (this != &other) {
      if (module_) {
        BinaryenModuleDispose(module_);
      }
      module_ = other.module_;
      other.module_ = nullptr;
    }
    return *this;
  }

  [[nodiscard]] auto get() const noexcept -> BinaryenModuleRef { return module_; }

  /// validate the module
  [[nodiscard]] auto validate() const -> bool { return BinaryenModuleValidate(module_) != 0; }

  /// optimize the module
  void optimize() { BinaryenModuleOptimize(module_); }

  /// emit to binary format
  [[nodiscard]] auto emit_binary() const -> std::vector<std::uint8_t> {
    auto result = BinaryenModuleAllocateAndWrite(module_, nullptr);
    std::vector<std::uint8_t> binary(static_cast<const std::uint8_t*>(result.binary),
                                     static_cast<const std::uint8_t*>(result.binary) +
                                         result.binaryBytes);
    free(result.binary);
    return binary;
  }

  /// emit to text format (WAT)
  [[nodiscard]] auto emit_text() const -> std::string {
    auto text = BinaryenModuleAllocateAndWriteText(module_);
    std::string result(text);
    free(text);
    return result;
  }

private:
  BinaryenModuleRef module_;
};

/// compiler state for a single compilation unit
class compiler {
public:
  explicit compiler(const ast::symbol_table& symbols) : symbols_(symbols) { setup_module(); }

  /// compile an expression to WASM
  /// returns a module that exports a single "main" function
  [[nodiscard]] auto compile(const ast::expression& expr) -> wasm_module {
    // create top-level scope
    lexical_scope top_scope;
    current_scope_ = &top_scope;

    // create a lambda context for main to allow local variables (for let expressions, etc.)
    current_lambda_context_ = lambda_context{};

    // compile the expression to a WASM expression
    auto wasm_expr = compile_expression(expr);

    // get local types from the context
    auto& local_types = current_lambda_context_->local_types;

    // wrap in a function
    auto result_type = make_nix_value_type();
    BinaryenAddFunction(module_.get(), "main",
                        BinaryenTypeNone(), // no parameters
                        result_type, local_types.empty() ? nullptr : local_types.data(),
                        static_cast<BinaryenIndex>(local_types.size()), wasm_expr);

    // clear the lambda context
    current_lambda_context_ = std::nullopt;

    // export the main function
    BinaryenAddFunctionExport(module_.get(), "main", "main");

    // add function table for indirect calls (lambdas and thunks)
    // lambdas have signature (i32, i64) -> i64
    // thunks have signature (i32) -> i64
    // both are indexed separately: lambdas at indices [0, lambda_count),
    // thunks at indices [lambda_count, lambda_count + thunk_count)
    if (!lambda_function_names_.empty() || !thunk_function_names_.empty()) {
      std::vector<const char*> func_names;
      func_names.reserve(lambda_function_names_.size() + thunk_function_names_.size());
      for (const auto& name : lambda_function_names_) {
        func_names.push_back(name.c_str());
      }
      for (const auto& name : thunk_function_names_) {
        func_names.push_back(name.c_str());
      }
      BinaryenAddTable(module_.get(), "functions", static_cast<BinaryenIndex>(func_names.size()),
                       static_cast<BinaryenIndex>(func_names.size()), BinaryenTypeFuncref());
      BinaryenAddActiveElementSegment(module_.get(), "functions", "functions", func_names.data(),
                                      static_cast<BinaryenIndex>(func_names.size()),
                                      BinaryenConst(module_.get(), BinaryenLiteralInt32(0)));
      // Export the function table for indirect calls from the host
      BinaryenAddTableExport(module_.get(), "functions", "functions");

      // Export the lambda count as a global so the runtime can compute thunk table indices
      // Thunks use relative indices (0, 1, 2...) but are stored at [lambda_count, ...) in table
      auto lambda_count = static_cast<std::int32_t>(lambda_function_names_.size());
      BinaryenAddGlobal(module_.get(), "__lambda_count", BinaryenTypeInt32(), false,
                        BinaryenConst(module_.get(), BinaryenLiteralInt32(lambda_count)));
      BinaryenAddGlobalExport(module_.get(), "__lambda_count", "__lambda_count");
    }

    current_scope_ = nullptr;
    return std::move(module_);
  }

private:
  const ast::symbol_table& symbols_;
  wasm_module module_;

  // string intern table index -> WASM data offset
  std::unordered_map<std::uint32_t, std::uint32_t> string_offsets_;
  std::uint32_t data_offset_ = 0;

  // current lexical scope during compilation
  lexical_scope* current_scope_ = nullptr;

  // lambda function counter and names for function table
  std::uint32_t lambda_counter_ = 0;
  std::vector<std::string> lambda_function_names_;

  // thunk function counter and names
  std::uint32_t thunk_counter_ = 0;
  std::vector<std::string> thunk_function_names_;

  // context for compiling a lambda body
  struct lambda_context {
    std::vector<BinaryenType> local_types;
    std::uint32_t next_local_index = 0;
    // captured variables from enclosing scope, in order
    std::vector<ast::symbol> captures;
    // map from symbol to capture index
    std::unordered_map<ast::symbol, std::uint32_t, symbol_hash> capture_indices;
  };
  std::optional<lambda_context> current_lambda_context_;

  // with scope: stores the local index containing the namespace attrset
  struct with_scope {
    std::uint32_t namespace_local_index;
  };
  // stack of with scopes (innermost last)
  std::vector<with_scope> with_scopes_;

  /// set up the module with imports and types
  void setup_module() {
    // import memory from host
    BinaryenAddMemoryImport(module_.get(), "memory", "env", "memory",
                            0); // not shared

    // add builtin imports
    setup_builtin_imports();
  }

  /// set up host function imports for builtins
  void setup_builtin_imports() {
    auto nix_value_type = make_nix_value_type();

    // binary builtins: (nix_value, nix_value) -> nix_value
    BinaryenType binary_param_types[] = {nix_value_type, nix_value_type};
    auto binary_params = BinaryenTypeCreate(binary_param_types, 2);

    // arithmetic with position for type errors: (nix_value, nix_value, line: i32, col: i32) ->
    // nix_value
    BinaryenType arith_param_types[] = {nix_value_type, nix_value_type, BinaryenTypeInt32(),
                                        BinaryenTypeInt32()};
    auto arith_params = BinaryenTypeCreate(arith_param_types, 4);
    BinaryenAddFunctionImport(module_.get(), "__add", "builtins", "__add", arith_params,
                              nix_value_type);
    BinaryenAddFunctionImport(module_.get(), "__sub", "builtins", "__sub", arith_params,
                              nix_value_type);
    BinaryenAddFunctionImport(module_.get(), "__mul", "builtins", "__mul", arith_params,
                              nix_value_type);
    BinaryenAddFunctionImport(module_.get(), "__div", "builtins", "__div", arith_params,
                              nix_value_type);

    // comparison (no position needed - should not throw type errors in most cases)
    BinaryenAddFunctionImport(module_.get(), "__lessThan", "builtins", "__lessThan", binary_params,
                              nix_value_type);
    BinaryenAddFunctionImport(module_.get(), "__lessEq", "builtins", "__lessEq", binary_params,
                              nix_value_type);
    BinaryenAddFunctionImport(module_.get(), "__eq", "builtins", "__eq", binary_params,
                              nix_value_type);
    BinaryenAddFunctionImport(module_.get(), "__neq", "builtins", "__neq", binary_params,
                              nix_value_type);

    // attrset/list operations
    BinaryenAddFunctionImport(module_.get(), "__update", "builtins", "__update", binary_params,
                              nix_value_type);
    BinaryenAddFunctionImport(module_.get(), "__concat", "builtins", "__concat", binary_params,
                              nix_value_type);

    // unary builtins: (nix_value) -> nix_value
    BinaryenType unary_param_types[] = {nix_value_type};
    auto unary_params = BinaryenTypeCreate(unary_param_types, 1);

    BinaryenAddFunctionImport(module_.get(), "__not", "builtins", "__not", unary_params,
                              nix_value_type);
    BinaryenAddFunctionImport(module_.get(), "__negate", "builtins", "__negate", unary_params,
                              nix_value_type);
    BinaryenAddFunctionImport(module_.get(), "__force", "runtime", "__force", unary_params,
                              nix_value_type);

    // type checking
    BinaryenAddFunctionImport(module_.get(), "__isBool", "builtins", "__isBool", unary_params,
                              BinaryenTypeInt32());

    // list construction: (i32 count, ...elements) - variadic, use memory
    // __makeList(offset: i32, count: i32) -> nix_value
    BinaryenType make_list_params[] = {BinaryenTypeInt32(), BinaryenTypeInt32()};
    BinaryenAddFunctionImport(module_.get(), "__makeList", "builtins", "__makeList",
                              BinaryenTypeCreate(make_list_params, 2), nix_value_type);

    // attrset construction: __makeAttrs(offset: i32, count: i32) -> nix_value
    BinaryenAddFunctionImport(module_.get(), "__makeAttrs", "builtins", "__makeAttrs",
                              BinaryenTypeCreate(make_list_params, 2), nix_value_type);

    // function application: __apply(fn: nix_value, arg: nix_value) -> nix_value
    BinaryenAddFunctionImport(module_.get(), "__apply", "runtime", "__apply", binary_params,
                              nix_value_type);

    // attribute selection: __select(set: nix_value, key_offset: i32, line: i32, col: i32) ->
    // nix_value
    BinaryenType select_params[] = {nix_value_type, BinaryenTypeInt32(), BinaryenTypeInt32(),
                                    BinaryenTypeInt32()};
    BinaryenAddFunctionImport(module_.get(), "__select", "builtins", "__select",
                              BinaryenTypeCreate(select_params, 4), nix_value_type);

    // has attribute: __hasAttr(set: nix_value, key_offset: i32) -> nix_value (bool)
    // (no position needed - doesn't throw on missing attribute)
    BinaryenType has_attr_params[] = {nix_value_type, BinaryenTypeInt32()};
    BinaryenAddFunctionImport(module_.get(), "__hasAttr", "builtins", "__hasAttr",
                              BinaryenTypeCreate(has_attr_params, 2), nix_value_type);

    // dynamic attribute selection: __selectDynamic(set: nix_value, key: nix_value, line: i32, col:
    // i32) -> nix_value
    BinaryenType select_dynamic_params[] = {nix_value_type, nix_value_type, BinaryenTypeInt32(),
                                            BinaryenTypeInt32()};
    BinaryenAddFunctionImport(module_.get(), "__selectDynamic", "builtins", "__selectDynamic",
                              BinaryenTypeCreate(select_dynamic_params, 4), nix_value_type);

    // dynamic has attribute: __hasAttrDynamic(set: nix_value, key: nix_value) -> nix_value (bool)
    // (no position needed - doesn't throw on missing attribute)
    BinaryenAddFunctionImport(module_.get(), "__hasAttrDynamic", "builtins", "__hasAttrDynamic",
                              binary_params, nix_value_type);

    // dynamic attrset construction: __makeAttrsDynamic(offset: i32, count: i32) -> nix_value
    // layout: pairs of (key: nix_value, value: nix_value) at offset
    BinaryenAddFunctionImport(module_.get(), "__makeAttrsDynamic", "builtins", "__makeAttrsDynamic",
                              BinaryenTypeCreate(make_list_params, 2), nix_value_type);

    // throw/assert: __throw(msg_offset: i32, line: i32, col: i32) -> nix_value (never returns)
    BinaryenType throw_params[] = {BinaryenTypeInt32(), BinaryenTypeInt32(), BinaryenTypeInt32()};
    BinaryenAddFunctionImport(module_.get(), "__throw", "runtime", "__throw",
                              BinaryenTypeCreate(throw_params, 3), nix_value_type);

    // variable lookup for free variables: __lookupVar(name_offset: i32) -> nix_value
    BinaryenAddFunctionImport(module_.get(), "__lookupVar", "runtime", "__lookupVar",
                              BinaryenTypeCreate(throw_params, 1), nix_value_type);

    // closure creation: __makeClosure(func_index: i32, env_offset: i32, env_size: i32) -> nix_value
    BinaryenType closure_params[] = {BinaryenTypeInt32(), BinaryenTypeInt32(), BinaryenTypeInt32()};
    BinaryenAddFunctionImport(module_.get(), "__makeClosure", "runtime", "__makeClosure",
                              BinaryenTypeCreate(closure_params, 3), nix_value_type);

    // thunk creation: __makeThunk(func_index: i32, env_offset: i32, env_size: i32) -> nix_value
    // thunks are similar to closures but are forced (memoized) on first access
    BinaryenAddFunctionImport(module_.get(), "__makeThunk", "runtime", "__makeThunk",
                              BinaryenTypeCreate(closure_params, 3), nix_value_type);

    // string coercion: __toString(nix_value) -> nix_value (string)
    BinaryenAddFunctionImport(module_.get(), "__toString", "builtins", "__toString", unary_params,
                              nix_value_type);

    // string concatenation: __concatStrings(offset: i32, count: i32) -> nix_value
    BinaryenType concat_strings_params[] = {BinaryenTypeInt32(), BinaryenTypeInt32()};
    BinaryenAddFunctionImport(module_.get(), "__concatStrings", "builtins", "__concatStrings",
                              BinaryenTypeCreate(concat_strings_params, 2), nix_value_type);
  }

  /// create the nix_value struct type (i32, i32)
  [[nodiscard]] auto make_nix_value_type() -> BinaryenType {
    // for now, represent nix_value as i64 (tag in low 32 bits, payload in high 32)
    // TODO: use WASM GC structs when available
    return BinaryenTypeInt64();
  }

  /// compile an expression to a WASM expression
  [[nodiscard]] auto compile_expression(const ast::expression& expr) -> BinaryenExpressionRef {
    return std::visit([this](const auto& e) { return compile_variant(e); }, expr->data);
  }

  /// compile an expression as a thunk (lazy evaluation)
  /// This creates a function that evaluates the expression when called,
  /// capturing any free variables from the current scope.
  /// Returns a nix_value with tag=thunk.
  [[nodiscard]] auto compile_as_thunk(const ast::expression& expr) -> BinaryenExpressionRef {
    // generate a unique function name for this thunk
    auto func_index = thunk_counter_++;
    auto func_name = "__thunk_" + std::to_string(func_index);
    thunk_function_names_.push_back(func_name);

    // analyze free variables in the expression
    std::vector<ast::symbol> bound_names; // thunks have no bound parameters
    auto free_vars = free_variable_analyzer::analyze(expr, bound_names);

    // filter free variables: only keep those that are actually in scope
    std::vector<ast::symbol> captures;
    for (auto sym : free_vars) {
      if (current_scope_ && current_scope_->lookup(sym).has_value()) {
        captures.push_back(sym);
      }
    }

    // save current lambda context
    auto outer_lambda_context = std::move(current_lambda_context_);
    current_lambda_context_ = lambda_context{};
    current_lambda_context_->captures = captures;

    // build capture index map
    for (std::uint32_t i = 0; i < captures.size(); ++i) {
      current_lambda_context_->capture_indices[captures[i]] = i;
    }

    // create a new scope for the thunk body
    std::uint32_t new_depth = current_scope_ ? current_scope_->depth() + 1 : 1;
    lexical_scope thunk_scope(nullptr, new_depth);
    auto* outer_scope = current_scope_;
    current_scope_ = &thunk_scope;

    // thunk functions take only (env_ptr: i32) -> nix_value
    // local 0: env_ptr (pointer to thunk environment)
    current_lambda_context_->local_types.push_back(BinaryenTypeInt32()); // env_ptr
    current_lambda_context_->next_local_index = 1;

    // add captured variables to the scope
    for (std::uint32_t i = 0; i < captures.size(); ++i) {
      thunk_scope.add_captured(captures[i], i);
    }

    // compile the body
    auto body_expr = compile_expression(expr);

    // create the function
    // params: (env_ptr: i32)
    BinaryenType param_types[] = {BinaryenTypeInt32()};
    auto params = BinaryenTypeCreate(param_types, 1);

    // locals: skip the first 1 (env_ptr param), the rest are actual locals
    std::vector<BinaryenType> local_types;
    for (std::size_t i = 1; i < current_lambda_context_->local_types.size(); ++i) {
      local_types.push_back(current_lambda_context_->local_types[i]);
    }

    BinaryenAddFunction(module_.get(), func_name.c_str(), params, make_nix_value_type(),
                        local_types.empty() ? nullptr : local_types.data(),
                        static_cast<BinaryenIndex>(local_types.size()), body_expr);

    // restore scope and context
    current_scope_ = outer_scope;
    auto captured_vars = std::move(current_lambda_context_->captures);
    current_lambda_context_ = std::move(outer_lambda_context);

    // create thunk value
    if (captured_vars.empty()) {
      // no captures - create a simple thunk with null env
      // call __makeThunk(func_index, 0, 0)
      BinaryenExpressionRef make_thunk_args[] = {
          BinaryenConst(module_.get(), BinaryenLiteralInt32(static_cast<std::int32_t>(func_index))),
          BinaryenConst(module_.get(), BinaryenLiteralInt32(0)),
          BinaryenConst(module_.get(), BinaryenLiteralInt32(0))};
      return BinaryenCall(module_.get(), "__makeThunk", make_thunk_args, 3, make_nix_value_type());
    }

    // has captures - need to allocate environment and store captured values
    // env layout: capture_count (i32) + captures[N] (nix_value each)
    // total size: 4 + N * 8 bytes
    auto capture_count = static_cast<std::uint32_t>(captured_vars.size());
    auto env_size = 4 + capture_count * 8;

    // allocate env memory
    auto env_offset = data_offset_;
    data_offset_ += env_size;
    data_offset_ = (data_offset_ + 7) & ~7u; // align to 8

    // generate code to:
    // 1. store capture_count at env_offset
    // 2. store each captured value at env_offset + 4 + i*8
    // 3. call __makeThunk
    std::vector<BinaryenExpressionRef> thunk_setup;

    // store capture_count
    thunk_setup.push_back(BinaryenStore(
        module_.get(), 4, env_offset, 0, BinaryenConst(module_.get(), BinaryenLiteralInt32(0)),
        BinaryenConst(module_.get(),
                      BinaryenLiteralInt32(static_cast<std::int32_t>(capture_count))),
        BinaryenTypeInt32(), "memory"));

    // store each captured value
    for (std::uint32_t i = 0; i < capture_count; ++i) {
      auto sym = captured_vars[i];
      // look up the variable in the outer scope to get its value
      auto var_value = compile_identifier_lookup(sym);
      thunk_setup.push_back(BinaryenStore(module_.get(), 8, env_offset + 4 + i * 8, 0,
                                          BinaryenConst(module_.get(), BinaryenLiteralInt32(0)),
                                          var_value, BinaryenTypeInt64(), "memory"));
    }

    // call __makeThunk(func_index, env_offset, env_size)
    BinaryenExpressionRef make_thunk_args[] = {
        BinaryenConst(module_.get(), BinaryenLiteralInt32(static_cast<std::int32_t>(func_index))),
        BinaryenConst(module_.get(), BinaryenLiteralInt32(static_cast<std::int32_t>(env_offset))),
        BinaryenConst(module_.get(), BinaryenLiteralInt32(static_cast<std::int32_t>(env_size)))};
    auto make_thunk =
        BinaryenCall(module_.get(), "__makeThunk", make_thunk_args, 3, make_nix_value_type());

    thunk_setup.push_back(make_thunk);
    return BinaryenBlock(module_.get(), nullptr, thunk_setup.data(),
                         static_cast<BinaryenIndex>(thunk_setup.size()), make_nix_value_type());
  }

  /// compile integer literal
  [[nodiscard]] auto compile_variant(const ast::expression_integer& expr) -> BinaryenExpressionRef {
    // pack tag=2 (int) and value into i64
    // low 32 bits: tag, high 32 bits: value (truncated)
    std::int64_t packed = (static_cast<std::int64_t>(expr.value) << 32) |
                          static_cast<std::int64_t>(value_tag::integer);
    return BinaryenConst(module_.get(), BinaryenLiteralInt64(packed));
  }

  /// compile float literal
  [[nodiscard]] auto compile_variant(const ast::expression_float& expr) -> BinaryenExpressionRef {
    // floats are stored as 64-bit IEEE 754 doubles
    // we reinterpret the double bits as the payload
    std::uint64_t bits;
    std::memcpy(&bits, &expr.value, sizeof(bits));

    // pack tag=3 (float) and the double bits
    // since we need full 64 bits for the double, we store it in a data segment
    // and return a pointer to it
    auto offset = data_offset_;
    std::vector<char> data(8);
    std::memcpy(data.data(), &expr.value, 8);

    BinaryenAddDataSegment(module_.get(), nullptr, "memory", false,
                           BinaryenConst(module_.get(), BinaryenLiteralInt32(offset)), data.data(),
                           8);
    data_offset_ += 8;

    // pack tag=3 (float) and offset into i64
    std::int64_t packed =
        (static_cast<std::int64_t>(offset) << 32) | static_cast<std::int64_t>(value_tag::floating);
    return BinaryenConst(module_.get(), BinaryenLiteralInt64(packed));
  }

  /// compile string literal
  [[nodiscard]] auto compile_variant(const ast::expression_string& expr) -> BinaryenExpressionRef {
    // allocate string data in data segment
    auto offset = allocate_string(expr.value);

    // pack tag=4 (string) and offset into i64
    std::int64_t packed =
        (static_cast<std::int64_t>(offset) << 32) | static_cast<std::int64_t>(value_tag::string);
    return BinaryenConst(module_.get(), BinaryenLiteralInt64(packed));
  }

  /// compile identifier reference
  [[nodiscard]] auto compile_variant(const ast::expression_identifier& expr)
      -> BinaryenExpressionRef {
    return compile_identifier_lookup(expr.name, expr.position);
  }

  /// compile identifier lookup - shared between direct identifier references and closure capture
  /// position is used for error reporting when looking up in with scopes
  [[nodiscard]] auto compile_identifier_lookup(ast::symbol name,
                                               ast::source_position position = {0, 0, 0})
      -> BinaryenExpressionRef {
    // first check the current scope hierarchy for local variables
    if (current_scope_) {
      auto lookup_result = current_scope_->lookup(name);
      if (lookup_result.has_value()) {
        const auto& local_binding = lookup_result->first;
        if (local_binding.location == variable_location::local) {
          // local variable - read from WASM local
          // IMPORTANT: we must force the value because let bindings store thunks
          // that need to be evaluated when accessed
          auto local_value =
              BinaryenLocalGet(module_.get(), local_binding.local_index, make_nix_value_type());
          return compile_force(local_value);
        } else {
          // captured variable - read from closure environment
          // env_ptr is local 0 and already points to the captures area (closure_ptr + 8)
          // captures are at env_ptr + capture_index * 8
          auto env_ptr = BinaryenLocalGet(module_.get(), 0, BinaryenTypeInt32());
          auto capture_offset = local_binding.capture_index * 8;
          auto captured_value = BinaryenLoad(module_.get(), 8, 0, capture_offset, 0,
                                             BinaryenTypeInt64(), env_ptr, "memory");
          // Also force captured variables - they might also be thunks
          return compile_force(captured_value);
        }
      }
    }

    // check if this variable is being captured from an outer scope
    // (this happens when compiling the closure creation, not the lambda body)
    if (current_lambda_context_.has_value()) {
      auto capture_it = current_lambda_context_->capture_indices.find(name);
      if (capture_it != current_lambda_context_->capture_indices.end()) {
        // this variable is in our capture list - read from closure environment
        // env_ptr is local 0 and already points to the captures area (closure_ptr + 8)
        auto env_ptr = BinaryenLocalGet(module_.get(), 0, BinaryenTypeInt32());
        auto capture_offset = capture_it->second * 8;
        return BinaryenLoad(module_.get(), 8, 0, capture_offset, 0, BinaryenTypeInt64(), env_ptr,
                            "memory");
      }
    }

    // check with scopes (innermost to outermost)
    // generate nested if-else: if (hasAttr(ns1, name)) select(ns1, name) else if (hasAttr(ns2,
    // name)) ...
    if (!with_scopes_.empty()) {
      auto name_str = symbols_.lookup(name);
      auto name_offset = allocate_string(name_str);

      // start with the fallback: __lookupVar(name)
      BinaryenExpressionRef fallback_args[] = {BinaryenConst(
          module_.get(), BinaryenLiteralInt32(static_cast<std::int32_t>(name_offset)))};
      BinaryenExpressionRef result =
          BinaryenCall(module_.get(), "__lookupVar", fallback_args, 1, make_nix_value_type());

      // iterate from outermost to innermost (reverse order) to build nested if-else
      for (auto it = with_scopes_.rbegin(); it != with_scopes_.rend(); ++it) {
        auto namespace_value =
            BinaryenLocalGet(module_.get(), it->namespace_local_index, make_nix_value_type());

        // __hasAttr(namespace, name_offset) -> bool
        BinaryenExpressionRef has_attr_args[] = {
            namespace_value,
            BinaryenConst(module_.get(),
                          BinaryenLiteralInt32(static_cast<std::int32_t>(name_offset)))};
        auto has_attr_result =
            BinaryenCall(module_.get(), "__hasAttr", has_attr_args, 2, make_nix_value_type());

        // check if hasAttr returned true
        auto has_attr_is_true =
            BinaryenBinary(module_.get(), BinaryenEqInt64(),
                           BinaryenConst(module_.get(), BinaryenLiteralInt64(packed::boolean_true)),
                           has_attr_result);

        // __select(namespace, name_offset, line, col) -> value
        // need to re-get namespace since the previous one was consumed
        auto namespace_value2 =
            BinaryenLocalGet(module_.get(), it->namespace_local_index, make_nix_value_type());
        auto select_result = compile_select(namespace_value2, name_str, position);

        // if hasAttr then select else (previous result)
        result = BinaryenIf(module_.get(), has_attr_is_true, select_result, result);
      }

      return result;
    }

    // no with scopes - variable not found in any scope, this is a free variable (e.g., builtin)
    // call a runtime function to look up the builtin
    auto name_str = symbols_.lookup(name);
    auto name_offset = allocate_string(name_str);

    BinaryenExpressionRef args[] = {
        BinaryenConst(module_.get(), BinaryenLiteralInt32(static_cast<std::int32_t>(name_offset)))};
    return BinaryenCall(module_.get(), "__lookupVar", args, 1, make_nix_value_type());
  }

  /// compile binary operation
  [[nodiscard]] auto compile_variant(const ast::expression_binary_operation& expr)
      -> BinaryenExpressionRef {
    // short-circuit evaluation for logical operators
    if (expr.op == ast::binary_operator::logical_and) {
      return compile_logical_and(expr);
    }
    if (expr.op == ast::binary_operator::logical_or) {
      return compile_logical_or(expr);
    }
    if (expr.op == ast::binary_operator::logical_implies) {
      return compile_logical_implies(expr);
    }
    if (expr.op == ast::binary_operator::pipe_right) {
      // a |> b is equivalent to b a
      return compile_pipe_right(expr);
    }
    if (expr.op == ast::binary_operator::pipe_left) {
      // a <| b is equivalent to a b
      return compile_pipe_left(expr);
    }

    auto left = compile_expression(expr.left);
    auto right = compile_expression(expr.right);

    // select the appropriate builtin
    const char* builtin = nullptr;
    switch (expr.op) {
      case ast::binary_operator::add:
        builtin = "__add";
        break;
      case ast::binary_operator::subtract:
        builtin = "__sub";
        break;
      case ast::binary_operator::multiply:
        builtin = "__mul";
        break;
      case ast::binary_operator::divide:
        builtin = "__div";
        break;
      case ast::binary_operator::less_than:
        builtin = "__lessThan";
        break;
      case ast::binary_operator::less_than_or_equal:
        builtin = "__lessEq";
        break;
      case ast::binary_operator::greater_than:
        // a > b is equivalent to b < a
        std::swap(left, right);
        builtin = "__lessThan";
        break;
      case ast::binary_operator::greater_than_or_equal:
        // a >= b is equivalent to b <= a
        std::swap(left, right);
        builtin = "__lessEq";
        break;
      case ast::binary_operator::equals:
        builtin = "__eq";
        break;
      case ast::binary_operator::not_equals:
        builtin = "__neq";
        break;
      case ast::binary_operator::update:
        builtin = "__update";
        break;
      case ast::binary_operator::concatenate:
        builtin = "__concat";
        break;
      default:
        throw compilation_error("unsupported binary operator");
    }

    // arithmetic ops need position for type error reporting
    bool is_arithmetic =
        (expr.op == ast::binary_operator::add || expr.op == ast::binary_operator::subtract ||
         expr.op == ast::binary_operator::multiply || expr.op == ast::binary_operator::divide);
    if (is_arithmetic) {
      BinaryenExpressionRef args[] = {
          left, right,
          BinaryenConst(module_.get(),
                        BinaryenLiteralInt32(static_cast<std::int32_t>(expr.position.line))),
          BinaryenConst(module_.get(),
                        BinaryenLiteralInt32(static_cast<std::int32_t>(expr.position.column)))};
      return BinaryenCall(module_.get(), builtin, args, 4, make_nix_value_type());
    }

    BinaryenExpressionRef args[] = {left, right};
    return BinaryenCall(module_.get(), builtin, args, 2, make_nix_value_type());
  }

  /// compile logical AND with short-circuit evaluation
  [[nodiscard]] auto compile_logical_and(const ast::expression_binary_operation& expr)
      -> BinaryenExpressionRef {
    auto left = compile_expression(expr.left);
    auto right = compile_expression(expr.right);

    // force the left operand (it might be a thunk)
    auto forced_left = compile_force(left);

    // if left is false, return false; otherwise return right
    // note: right is not forced here (short-circuit: only evaluated if needed)
    auto left_is_true = BinaryenBinary(
        module_.get(), BinaryenEqInt64(),
        BinaryenConst(module_.get(), BinaryenLiteralInt64(packed::boolean_true)), forced_left);

    return BinaryenIf(module_.get(), left_is_true, right,
                      BinaryenConst(module_.get(), BinaryenLiteralInt64(packed::boolean_false)));
  }

  /// compile logical OR with short-circuit evaluation
  [[nodiscard]] auto compile_logical_or(const ast::expression_binary_operation& expr)
      -> BinaryenExpressionRef {
    auto left = compile_expression(expr.left);
    auto right = compile_expression(expr.right);

    // force the left operand (it might be a thunk)
    auto forced_left = compile_force(left);

    // if left is true, return true; otherwise return right
    // note: right is not forced here (short-circuit: only evaluated if needed)
    auto left_is_true = BinaryenBinary(
        module_.get(), BinaryenEqInt64(),
        BinaryenConst(module_.get(), BinaryenLiteralInt64(packed::boolean_true)), forced_left);

    return BinaryenIf(module_.get(), left_is_true,
                      BinaryenConst(module_.get(), BinaryenLiteralInt64(packed::boolean_true)),
                      right);
  }

  /// compile logical implies: a -> b is equivalent to !a || b
  [[nodiscard]] auto compile_logical_implies(const ast::expression_binary_operation& expr)
      -> BinaryenExpressionRef {
    auto left = compile_expression(expr.left);
    auto right = compile_expression(expr.right);

    // force the left operand (it might be a thunk)
    auto forced_left = compile_force(left);

    // if left is false, return true; otherwise return right
    auto left_is_false = BinaryenBinary(
        module_.get(), BinaryenEqInt64(),
        BinaryenConst(module_.get(), BinaryenLiteralInt64(packed::boolean_false)), forced_left);

    return BinaryenIf(module_.get(), left_is_false,
                      BinaryenConst(module_.get(), BinaryenLiteralInt64(packed::boolean_true)),
                      right);
  }

  /// compile pipe right: a |> b is b(a)
  [[nodiscard]] auto compile_pipe_right(const ast::expression_binary_operation& expr)
      -> BinaryenExpressionRef {
    auto arg = compile_expression(expr.left);
    auto func = compile_expression(expr.right);
    BinaryenExpressionRef args[] = {func, arg};
    return BinaryenCall(module_.get(), "__apply", args, 2, make_nix_value_type());
  }

  /// compile pipe left: a <| b is a(b)
  [[nodiscard]] auto compile_pipe_left(const ast::expression_binary_operation& expr)
      -> BinaryenExpressionRef {
    auto func = compile_expression(expr.left);
    auto arg = compile_expression(expr.right);
    BinaryenExpressionRef args[] = {func, arg};
    return BinaryenCall(module_.get(), "__apply", args, 2, make_nix_value_type());
  }

  /// compile unary operation
  [[nodiscard]] auto compile_variant(const ast::expression_unary_operation& expr)
      -> BinaryenExpressionRef {
    auto operand = compile_expression(expr.operand);

    const char* builtin = nullptr;
    switch (expr.op) {
      case ast::unary_operator::logical_not:
        builtin = "__not";
        break;
      case ast::unary_operator::negate:
        builtin = "__negate";
        break;
    }

    BinaryenExpressionRef args[] = {operand};
    return BinaryenCall(module_.get(), builtin, args, 1, make_nix_value_type());
  }

  // string and path interpolation
  [[nodiscard]] auto compile_variant(const ast::expression_string_interpolated& expr)
      -> BinaryenExpressionRef {
    // string interpolation: concatenate parts
    // each part is either a literal string or an expression to stringify
    if (expr.parts.empty()) {
      // empty string
      auto offset = allocate_string("");
      std::int64_t packed =
          (static_cast<std::int64_t>(offset) << 32) | static_cast<std::int64_t>(value_tag::string);
      return BinaryenConst(module_.get(), BinaryenLiteralInt64(packed));
    }

    // optimization: if single literal part, just return that string
    if (expr.parts.size() == 1 && std::holds_alternative<std::string>(expr.parts[0])) {
      auto offset = allocate_string(std::get<std::string>(expr.parts[0]));
      std::int64_t packed =
          (static_cast<std::int64_t>(offset) << 32) | static_cast<std::int64_t>(value_tag::string);
      return BinaryenConst(module_.get(), BinaryenLiteralInt64(packed));
    }

    // general case: compile all parts, coerce to strings, concatenate at runtime
    return compile_interpolation_parts(expr.parts, value_tag::string);
  }

  [[nodiscard]] auto compile_variant(const ast::expression_path& expr) -> BinaryenExpressionRef {
    // paths are represented as strings with tag=5 (path)
    auto offset = allocate_string(expr.value);
    std::int64_t packed =
        (static_cast<std::int64_t>(offset) << 32) | static_cast<std::int64_t>(value_tag::path);
    return BinaryenConst(module_.get(), BinaryenLiteralInt64(packed));
  }

  [[nodiscard]] auto compile_variant(const ast::expression_path_interpolated& expr)
      -> BinaryenExpressionRef {
    // path interpolation: similar to string interpolation but returns path type
    if (expr.parts.empty()) {
      // empty path (unusual but handle it)
      auto offset = allocate_string("");
      std::int64_t packed =
          (static_cast<std::int64_t>(offset) << 32) | static_cast<std::int64_t>(value_tag::path);
      return BinaryenConst(module_.get(), BinaryenLiteralInt64(packed));
    }

    // optimization: if single literal part, just return that path
    if (expr.parts.size() == 1 && std::holds_alternative<std::string>(expr.parts[0])) {
      auto offset = allocate_string(std::get<std::string>(expr.parts[0]));
      std::int64_t packed =
          (static_cast<std::int64_t>(offset) << 32) | static_cast<std::int64_t>(value_tag::path);
      return BinaryenConst(module_.get(), BinaryenLiteralInt64(packed));
    }

    // general case: compile all parts, coerce to strings, concatenate at runtime
    // the result is tagged as a path instead of a string
    return compile_interpolation_parts(expr.parts, value_tag::path);
  }

  /// check if an expression is trivial (literal) and doesn't need a thunk
  [[nodiscard]] static auto is_trivial_expression(const ast::expression& expr) -> bool {
    return std::visit(
        [](const auto& e) -> bool {
          using T = std::decay_t<decltype(e)>;
          // literals don't need thunks - they're already values
          if constexpr (std::is_same_v<T, ast::expression_integer> ||
                        std::is_same_v<T, ast::expression_float> ||
                        std::is_same_v<T, ast::expression_string> ||
                        std::is_same_v<T, ast::expression_path>) {
            return true;
          }
          // identifiers might be thunks themselves, but looking them up is cheap
          if constexpr (std::is_same_v<T, ast::expression_identifier>) {
            return true;
          }
          return false;
        },
        expr->data);
  }

  [[nodiscard]] auto compile_variant(const ast::expression_list& expr) -> BinaryenExpressionRef {
    // compile all elements and store them in memory
    // then call __makeList(offset, count)
    // list elements are lazy - wrap non-trivial expressions in thunks
    auto count = static_cast<std::uint32_t>(expr.elements.size());

    if (count == 0) {
      // empty list is a constant
      std::int64_t packed = static_cast<std::int64_t>(value_tag::list);
      return BinaryenConst(module_.get(), BinaryenLiteralInt64(packed));
    }

    // allocate space for element nix_values (8 bytes each)
    auto offset = data_offset_;
    auto total_size = count * 8;
    data_offset_ += total_size;
    data_offset_ = (data_offset_ + 7) & ~7u; // align to 8

    // generate code to store each element
    std::vector<BinaryenExpressionRef> store_ops;
    for (std::uint32_t i = 0; i < count; ++i) {
      BinaryenExpressionRef element;
      if (is_trivial_expression(expr.elements[i])) {
        // trivial expressions can be evaluated immediately
        element = compile_expression(expr.elements[i]);
      } else {
        // non-trivial expressions are wrapped in thunks for lazy evaluation
        element = compile_as_thunk(expr.elements[i]);
      }
      auto store = BinaryenStore(module_.get(),
                                 8,                                                     // bytes
                                 offset + i * 8,                                        // offset
                                 0,                                                     // align
                                 BinaryenConst(module_.get(), BinaryenLiteralInt32(0)), // base
                                 element, BinaryenTypeInt64(), "memory");
      store_ops.push_back(store);
    }

    // call __makeList
    BinaryenExpressionRef make_list_args[] = {
        BinaryenConst(module_.get(), BinaryenLiteralInt32(static_cast<std::int32_t>(offset))),
        BinaryenConst(module_.get(), BinaryenLiteralInt32(static_cast<std::int32_t>(count)))};
    auto make_list =
        BinaryenCall(module_.get(), "__makeList", make_list_args, 2, make_nix_value_type());

    // combine: store all elements, then make list
    store_ops.push_back(make_list);
    return BinaryenBlock(module_.get(), nullptr, store_ops.data(),
                         static_cast<BinaryenIndex>(store_ops.size()), make_nix_value_type());
  }

  /// build a nested attrset value from a multi-segment path
  /// e.g., for path segments [b, c] and value v, creates { b = { c = v; }; }
  /// builds from innermost to outermost, starting at segments_begin
  [[nodiscard]] auto build_nested_attrset(const std::vector<ast::attribute_name>& segments,
                                          std::size_t segments_begin,
                                          BinaryenExpressionRef innermost_value)
      -> BinaryenExpressionRef {
    // base case: no more segments to process
    if (segments_begin >= segments.size()) {
      return innermost_value;
    }

    // build from innermost to outermost
    // start with the innermost value and wrap it in successive attrsets
    auto current_value = innermost_value;

    for (auto i = segments.size(); i > segments_begin; --i) {
      const auto& segment = segments[i - 1];

      if (segment.is_dynamic()) {
        throw compilation_error(
            "dynamic attribute names in multi-segment paths not yet implemented");
      }

      auto sym = std::get<ast::symbol>(segment.value);
      auto key_str = symbols_.lookup(sym);
      auto key_offset = allocate_string(key_str);

      // allocate space for a single-attribute attrset: 12 bytes (4 key + 8 value)
      auto pair_offset = data_offset_;
      data_offset_ += 12;
      data_offset_ = (data_offset_ + 7) & ~7u;

      std::vector<BinaryenExpressionRef> store_ops;

      // store key offset
      auto store_key = BinaryenStore(
          module_.get(), 4, pair_offset, 0, BinaryenConst(module_.get(), BinaryenLiteralInt32(0)),
          BinaryenConst(module_.get(), BinaryenLiteralInt32(static_cast<std::int32_t>(key_offset))),
          BinaryenTypeInt32(), "memory");
      store_ops.push_back(store_key);

      // store the current value
      auto store_value = BinaryenStore(module_.get(), 8, pair_offset + 4, 0,
                                       BinaryenConst(module_.get(), BinaryenLiteralInt32(0)),
                                       current_value, BinaryenTypeInt64(), "memory");
      store_ops.push_back(store_value);

      // call __makeAttrs to create a single-element attrset
      BinaryenExpressionRef make_attrs_args[] = {
          BinaryenConst(module_.get(),
                        BinaryenLiteralInt32(static_cast<std::int32_t>(pair_offset))),
          BinaryenConst(module_.get(), BinaryenLiteralInt32(1))};
      auto make_attrs =
          BinaryenCall(module_.get(), "__makeAttrs", make_attrs_args, 2, make_nix_value_type());

      store_ops.push_back(make_attrs);
      current_value =
          BinaryenBlock(module_.get(), nullptr, store_ops.data(),
                        static_cast<BinaryenIndex>(store_ops.size()), make_nix_value_type());
    }

    return current_value;
  }

  /// Represents a binding with a path slice (reference, not copy)
  /// segment_start: index into the original path where our slice begins (0 = first segment)
  struct binding_path_slice {
    const ast::binding_attribute* binding;
    std::size_t segment_start; // which segment index we're looking at
  };

  /// Group bindings by their first segment (at segment_start index) for multi-segment path merging
  /// Returns a map from segment symbol to list of binding slices
  [[nodiscard]] auto group_bindings_by_segment(const std::vector<binding_path_slice>& slices,
                                               std::size_t segment_index)
      -> std::unordered_map<ast::symbol, std::vector<binding_path_slice>, symbol_hash> {
    std::unordered_map<ast::symbol, std::vector<binding_path_slice>, symbol_hash> result;

    for (const auto& slice : slices) {
      const auto& segments = slice.binding->path.segments;
      if (segment_index >= segments.size()) {
        // This binding has no more segments at this level - it's a direct value
        // We handle this case separately
        continue;
      }

      const auto& segment = segments[segment_index];
      if (segment.is_dynamic()) {
        throw compilation_error("dynamic segment in path merging not supported");
      }

      auto sym = std::get<ast::symbol>(segment.value);
      result[sym].push_back({slice.binding, segment_index});
    }

    return result;
  }

  /// Group initial bindings by their first segment, preserving source order
  /// Returns a vector of pairs to maintain insertion order (important for let bindings)
  [[nodiscard]] auto
  group_bindings_by_first_segment(const std::vector<ast::binding_variant>& bindings)
      -> std::vector<std::pair<ast::symbol, std::vector<binding_path_slice>>> {
    // Use a map for grouping, but also track insertion order
    std::unordered_map<ast::symbol, std::size_t, symbol_hash> symbol_to_index;
    std::vector<std::pair<ast::symbol, std::vector<binding_path_slice>>> result;

    for (const auto& binding : bindings) {
      if (std::holds_alternative<ast::binding_attribute>(binding)) {
        const auto& attr_binding = std::get<ast::binding_attribute>(binding);
        if (attr_binding.path.segments.empty()) {
          throw compilation_error("empty attribute path");
        }

        const auto& first_segment = attr_binding.path.segments[0];
        if (first_segment.is_dynamic()) {
          throw compilation_error("dynamic first segment in path merging not supported");
        }

        auto first_sym = std::get<ast::symbol>(first_segment.value);
        auto it = symbol_to_index.find(first_sym);
        if (it == symbol_to_index.end()) {
          // New symbol - add to result and track index
          symbol_to_index[first_sym] = result.size();
          result.push_back({first_sym, {{&attr_binding, 0}}});
        } else {
          // Existing symbol - append to its group
          result[it->second].second.push_back({&attr_binding, 0});
        }
      }
    }

    return result;
  }

  /// Check if any binding has a dynamic segment
  [[nodiscard]] auto has_dynamic_bindings(const std::vector<ast::binding_variant>& bindings)
      -> bool {
    for (const auto& binding : bindings) {
      if (std::holds_alternative<ast::binding_attribute>(binding)) {
        const auto& attr_binding = std::get<ast::binding_attribute>(binding);
        for (const auto& seg : attr_binding.path.segments) {
          if (seg.is_dynamic()) {
            return true;
          }
        }
      }
    }
    return false;
  }

  /// Compile a merged attrset value for a group of binding slices that share a segment prefix
  /// segment_index: current depth in the path (0 = we just matched first segment, looking at
  /// second) This handles cases like { a.b = 1; a.c = 2; } by recursively building nested attrsets
  [[nodiscard]] auto compile_merged_attrset_value(const std::vector<binding_path_slice>& slices,
                                                  std::size_t segment_index)
      -> BinaryenExpressionRef {
    // Separate slices into:
    // 1. Direct values: slices where segment_index == path.size() (no more segments)
    // 2. Nested values: slices where segment_index < path.size() (more segments to process)
    const ast::expression* direct_value = nullptr;
    std::vector<binding_path_slice> nested_slices;

    for (const auto& slice : slices) {
      const auto& segments = slice.binding->path.segments;
      if (segment_index >= segments.size()) {
        // This slice ends here - it's a direct value
        if (direct_value != nullptr) {
          throw compilation_error("duplicate attribute definition");
        }
        direct_value = &slice.binding->value;
      } else {
        // More segments - needs nested handling
        nested_slices.push_back(slice);
      }
    }

    // If there's both a direct value and nested values, that's a conflict
    // e.g., { a = 1; a.b = 2; } - 'a' can't be both 1 and { b = 2; }
    if (direct_value != nullptr && !nested_slices.empty()) {
      throw compilation_error("attribute has both a direct value and nested attributes");
    }

    // If only direct value, return it (wrapped in thunk for lazy evaluation if non-trivial)
    if (direct_value != nullptr) {
      if (is_trivial_expression(*direct_value)) {
        return compile_expression(*direct_value);
      } else {
        return compile_as_thunk(*direct_value);
      }
    }

    // Group nested slices by the segment at segment_index
    std::unordered_map<ast::symbol, std::vector<binding_path_slice>, symbol_hash> nested_groups;

    for (const auto& slice : nested_slices) {
      const auto& segment = slice.binding->path.segments[segment_index];
      if (segment.is_dynamic()) {
        throw compilation_error("dynamic segment in path merging not supported");
      }
      auto sym = std::get<ast::symbol>(segment.value);
      nested_groups[sym].push_back(slice);
    }

    // Build the nested attrset from nested_groups
    auto attr_count = static_cast<std::uint32_t>(nested_groups.size());

    if (attr_count == 0) {
      return BinaryenConst(module_.get(), BinaryenLiteralInt64(packed::empty_attribute_set));
    }

    // Allocate space for (key_offset, value) pairs
    auto offset = data_offset_;
    auto total_size = attr_count * 12;
    data_offset_ += total_size;
    data_offset_ = (data_offset_ + 7) & ~7u;

    std::vector<BinaryenExpressionRef> store_ops;
    std::uint32_t pair_index = 0;

    for (const auto& [sym, group_slices] : nested_groups) {
      auto key_str = symbols_.lookup(sym);
      auto key_offset = allocate_string(key_str);

      auto pair_offset = offset + pair_index * 12;

      // Store key offset
      auto store_key = BinaryenStore(
          module_.get(), 4, pair_offset, 0, BinaryenConst(module_.get(), BinaryenLiteralInt32(0)),
          BinaryenConst(module_.get(), BinaryenLiteralInt32(static_cast<std::int32_t>(key_offset))),
          BinaryenTypeInt32(), "memory");
      store_ops.push_back(store_key);

      // Recursively compile the value (looking at the next segment)
      auto value = compile_merged_attrset_value(group_slices, segment_index + 1);

      auto store_value = BinaryenStore(module_.get(), 8, pair_offset + 4, 0,
                                       BinaryenConst(module_.get(), BinaryenLiteralInt32(0)), value,
                                       BinaryenTypeInt64(), "memory");
      store_ops.push_back(store_value);

      ++pair_index;
    }

    // Call __makeAttrs
    BinaryenExpressionRef make_attrs_args[] = {
        BinaryenConst(module_.get(), BinaryenLiteralInt32(static_cast<std::int32_t>(offset))),
        BinaryenConst(module_.get(), BinaryenLiteralInt32(static_cast<std::int32_t>(attr_count)))};
    auto make_attrs =
        BinaryenCall(module_.get(), "__makeAttrs", make_attrs_args, 2, make_nix_value_type());

    store_ops.push_back(make_attrs);
    return BinaryenBlock(module_.get(), nullptr, store_ops.data(),
                         static_cast<BinaryenIndex>(store_ops.size()), make_nix_value_type());
  }

  [[nodiscard]] auto compile_variant(const ast::expression_attribute_set& expr)
      -> BinaryenExpressionRef {
    // attribute sets are complex - we need to evaluate all bindings
    // and construct the set at runtime
    if (expr.is_recursive) {
      return compile_recursive_attribute_set(expr);
    }

    // Check for dynamic keys - use separate compilation path
    if (has_dynamic_bindings(expr.bindings)) {
      return compile_attribute_set_with_dynamic_keys(expr);
    }

    // Group bindings by their first segment for multi-segment path merging
    auto grouped = group_bindings_by_first_segment(expr.bindings);

    // Count total top-level attributes (from grouped bindings and inherit bindings)
    std::uint32_t attr_count = static_cast<std::uint32_t>(grouped.size());

    for (const auto& binding : expr.bindings) {
      if (std::holds_alternative<ast::binding_inherit>(binding)) {
        const auto& inherit_binding = std::get<ast::binding_inherit>(binding);
        attr_count += static_cast<std::uint32_t>(inherit_binding.attributes.size());
      }
    }

    if (attr_count == 0) {
      // empty attrset
      return BinaryenConst(module_.get(), BinaryenLiteralInt64(packed::empty_attribute_set));
    }

    // allocate space for (key_offset, value) pairs
    // each pair is 12 bytes: 4 bytes key offset + 8 bytes nix_value
    auto offset = data_offset_;
    auto total_size = attr_count * 12;
    data_offset_ += total_size;
    data_offset_ = (data_offset_ + 7) & ~7u;

    std::vector<BinaryenExpressionRef> store_ops;
    std::uint32_t pair_index = 0;

    // Process grouped attribute bindings (handles multi-segment path merging)
    for (const auto& [first_sym, slices] : grouped) {
      auto key_str = symbols_.lookup(first_sym);
      auto key_offset = allocate_string(key_str);

      auto pair_offset = offset + pair_index * 12;

      // store key offset
      auto store_key = BinaryenStore(
          module_.get(), 4, pair_offset, 0, BinaryenConst(module_.get(), BinaryenLiteralInt32(0)),
          BinaryenConst(module_.get(), BinaryenLiteralInt32(static_cast<std::int32_t>(key_offset))),
          BinaryenTypeInt32(), "memory");
      store_ops.push_back(store_key);

      // compile the (possibly merged) value
      // segment_index=1 because we already consumed the first segment (first_sym)
      auto value = compile_merged_attrset_value(slices, 1);

      auto store_value = BinaryenStore(module_.get(), 8, pair_offset + 4, 0,
                                       BinaryenConst(module_.get(), BinaryenLiteralInt32(0)), value,
                                       BinaryenTypeInt64(), "memory");
      store_ops.push_back(store_value);

      ++pair_index;
    }

    // Process inherit bindings
    for (const auto& binding : expr.bindings) {
      if (std::holds_alternative<ast::binding_inherit>(binding)) {
        const auto& inherit_binding = std::get<ast::binding_inherit>(binding);

        // if there's a from_expression, compile it once and select from it
        BinaryenExpressionRef from_value = nullptr;
        std::uint32_t from_local_index = 0;

        if (inherit_binding.from_expression.has_value()) {
          from_value = compile_expression(*inherit_binding.from_expression);

          // store the from_expression result in a temporary local to avoid re-evaluating
          if (current_lambda_context_.has_value() && inherit_binding.attributes.size() > 1) {
            from_local_index = current_lambda_context_->next_local_index++;
            current_lambda_context_->local_types.push_back(make_nix_value_type());
            auto store_from = BinaryenLocalSet(module_.get(), from_local_index, from_value);
            store_ops.push_back(store_from);
            from_value = nullptr; // will use local get instead
          }
        }

        for (const auto& attr_name : inherit_binding.attributes) {
          // for now only handle static attribute names
          if (attr_name.is_dynamic()) {
            throw compilation_error("dynamic inherit attribute names not yet implemented");
          }

          auto sym = std::get<ast::symbol>(attr_name.value);
          auto key_str = symbols_.lookup(sym);
          auto key_offset = allocate_string(key_str);

          // store key offset
          auto pair_offset = offset + pair_index * 12;
          auto store_key = BinaryenStore(
              module_.get(), 4, pair_offset, 0,
              BinaryenConst(module_.get(), BinaryenLiteralInt32(0)),
              BinaryenConst(module_.get(),
                            BinaryenLiteralInt32(static_cast<std::int32_t>(key_offset))),
              BinaryenTypeInt32(), "memory");
          store_ops.push_back(store_key);

          // get the value to store
          BinaryenExpressionRef value;
          if (inherit_binding.from_expression.has_value()) {
            // inherit (expr) x; -> select x from expr
            BinaryenExpressionRef source;
            if (from_value != nullptr) {
              // single attribute, use the compiled expression directly
              source = from_value;
              from_value = nullptr; // consumed
            } else {
              // multiple attributes, read from local
              source = BinaryenLocalGet(module_.get(), from_local_index, make_nix_value_type());
            }

            value = compile_select(source, key_str, attr_name.position);
          } else {
            // inherit x; -> look up x from outer scope
            value = compile_identifier_lookup(sym, attr_name.position);
          }

          auto store_value = BinaryenStore(module_.get(), 8, pair_offset + 4, 0,
                                           BinaryenConst(module_.get(), BinaryenLiteralInt32(0)),
                                           value, BinaryenTypeInt64(), "memory");
          store_ops.push_back(store_value);

          ++pair_index;
        }
      }
    }

    // call __makeAttrs
    BinaryenExpressionRef make_attrs_args[] = {
        BinaryenConst(module_.get(), BinaryenLiteralInt32(static_cast<std::int32_t>(offset))),
        BinaryenConst(module_.get(), BinaryenLiteralInt32(static_cast<std::int32_t>(attr_count)))};
    auto make_attrs =
        BinaryenCall(module_.get(), "__makeAttrs", make_attrs_args, 2, make_nix_value_type());

    store_ops.push_back(make_attrs);
    return BinaryenBlock(module_.get(), nullptr, store_ops.data(),
                         static_cast<BinaryenIndex>(store_ops.size()), make_nix_value_type());
  }

  /// compile attribute set with at least one dynamic key
  /// uses __makeAttrsDynamic which expects (key: nix_value, value: nix_value) pairs
  [[nodiscard]] auto
  compile_attribute_set_with_dynamic_keys(const ast::expression_attribute_set& expr)
      -> BinaryenExpressionRef {
    // count total attributes (single-segment only for now)
    std::uint32_t attr_count = 0;
    for (const auto& binding : expr.bindings) {
      if (std::holds_alternative<ast::binding_attribute>(binding)) {
        const auto& attr_binding = std::get<ast::binding_attribute>(binding);
        // for now, only single-segment paths are supported with dynamic keys
        if (attr_binding.path.segments.size() != 1) {
          throw compilation_error(
              "multi-segment attribute paths with dynamic keys not yet implemented");
        }
        ++attr_count;
      } else {
        throw compilation_error("inherit bindings with dynamic keys not yet implemented");
      }
    }

    if (attr_count == 0) {
      return BinaryenConst(module_.get(), BinaryenLiteralInt64(packed::empty_attribute_set));
    }

    // allocate space for (key: nix_value, value: nix_value) pairs
    // each pair is 16 bytes: 8 bytes key nix_value + 8 bytes value nix_value
    auto offset = data_offset_;
    auto total_size = attr_count * 16;
    data_offset_ += total_size;
    data_offset_ = (data_offset_ + 7) & ~7u;

    std::vector<BinaryenExpressionRef> store_ops;
    std::uint32_t pair_index = 0;

    for (const auto& binding : expr.bindings) {
      const auto& attr_binding = std::get<ast::binding_attribute>(binding);
      const auto& segment = attr_binding.path.segments[0];

      auto pair_offset = offset + pair_index * 16;

      // compile key - either static symbol or dynamic expression
      BinaryenExpressionRef key_value;
      if (segment.is_dynamic()) {
        // dynamic key: compile the expression to get a string value
        const auto& key_expr = std::get<ast::expression>(segment.value);
        key_value = compile_expression(key_expr);
      } else {
        // static key: create a string constant
        auto sym = std::get<ast::symbol>(segment.value);
        auto key_str = symbols_.lookup(sym);
        auto key_string_offset = allocate_string(key_str);
        std::int64_t packed = (static_cast<std::int64_t>(key_string_offset) << 32) |
                              static_cast<std::int64_t>(value_tag::string);
        key_value = BinaryenConst(module_.get(), BinaryenLiteralInt64(packed));
      }

      // store key nix_value
      auto store_key = BinaryenStore(module_.get(), 8, pair_offset, 0,
                                     BinaryenConst(module_.get(), BinaryenLiteralInt32(0)),
                                     key_value, BinaryenTypeInt64(), "memory");
      store_ops.push_back(store_key);

      // store value nix_value
      auto value = compile_expression(attr_binding.value);
      auto store_value = BinaryenStore(module_.get(), 8, pair_offset + 8, 0,
                                       BinaryenConst(module_.get(), BinaryenLiteralInt32(0)), value,
                                       BinaryenTypeInt64(), "memory");
      store_ops.push_back(store_value);

      ++pair_index;
    }

    // call __makeAttrsDynamic
    BinaryenExpressionRef make_attrs_args[] = {
        BinaryenConst(module_.get(), BinaryenLiteralInt32(static_cast<std::int32_t>(offset))),
        BinaryenConst(module_.get(), BinaryenLiteralInt32(static_cast<std::int32_t>(attr_count)))};
    auto make_attrs = BinaryenCall(module_.get(), "__makeAttrsDynamic", make_attrs_args, 2,
                                   make_nix_value_type());

    store_ops.push_back(make_attrs);
    return BinaryenBlock(module_.get(), nullptr, store_ops.data(),
                         static_cast<BinaryenIndex>(store_ops.size()), make_nix_value_type());
  }

  /// compile recursive attribute set: rec { a = 1; b = a + 1; }
  /// all bindings are in scope for all values
  [[nodiscard]] auto compile_recursive_attribute_set(const ast::expression_attribute_set& expr)
      -> BinaryenExpressionRef {
    // we need lambda context for locals
    if (!current_lambda_context_.has_value()) {
      throw compilation_error("recursive attribute sets require function context");
    }

    // first pass: collect all binding names and allocate locals for them
    struct rec_binding {
      ast::symbol name;
      std::uint32_t local_index;
      const ast::binding_attribute* attr_binding;  // null for inherit bindings
      const ast::binding_inherit* inherit_binding; // null for attr bindings
      std::size_t inherit_attr_index;              // which attribute in inherit binding
    };
    std::vector<rec_binding> bindings;

    // create a new scope for the recursive bindings
    std::uint32_t scope_depth = current_scope_ ? current_scope_->depth() : 0;
    lexical_scope rec_scope(current_scope_, scope_depth);
    auto* outer_scope = current_scope_;
    current_scope_ = &rec_scope;

    // first pass: allocate locals for all bindings
    for (const auto& binding : expr.bindings) {
      if (std::holds_alternative<ast::binding_attribute>(binding)) {
        const auto& attr_binding = std::get<ast::binding_attribute>(binding);

        // for now, only single-segment static paths
        if (attr_binding.path.segments.empty()) {
          current_scope_ = outer_scope;
          throw compilation_error("empty attribute path in rec");
        }
        if (attr_binding.path.segments.size() > 1) {
          current_scope_ = outer_scope;
          throw compilation_error("multi-segment attribute paths in rec not yet implemented");
        }
        const auto& segment = attr_binding.path.segments[0];
        if (segment.is_dynamic()) {
          current_scope_ = outer_scope;
          throw compilation_error("dynamic attribute names in rec not yet implemented");
        }

        auto sym = std::get<ast::symbol>(segment.value);
        auto local_index = current_lambda_context_->next_local_index++;
        current_lambda_context_->local_types.push_back(make_nix_value_type());

        rec_scope.add_local(sym, local_index);
        bindings.push_back({sym, local_index, &attr_binding, nullptr, 0});
      } else {
        const auto& inherit_binding = std::get<ast::binding_inherit>(binding);

        for (std::size_t i = 0; i < inherit_binding.attributes.size(); ++i) {
          const auto& attr_name = inherit_binding.attributes[i];
          if (attr_name.is_dynamic()) {
            current_scope_ = outer_scope;
            throw compilation_error("dynamic inherit names in rec not yet implemented");
          }

          auto sym = std::get<ast::symbol>(attr_name.value);
          auto local_index = current_lambda_context_->next_local_index++;
          current_lambda_context_->local_types.push_back(make_nix_value_type());

          rec_scope.add_local(sym, local_index);
          bindings.push_back({sym, local_index, nullptr, &inherit_binding, i});
        }
      }
    }

    std::vector<BinaryenExpressionRef> ops;

    // second pass: compile all values and store in locals
    // note: all bindings are already in scope, so they can reference each other
    for (const auto& binding : bindings) {
      BinaryenExpressionRef value;

      if (binding.attr_binding != nullptr) {
        // regular attribute binding
        value = compile_expression(binding.attr_binding->value);
      } else {
        // inherit binding
        const auto& inherit = *binding.inherit_binding;
        const auto& attr_name = inherit.attributes[binding.inherit_attr_index];
        auto sym = std::get<ast::symbol>(attr_name.value);

        if (inherit.from_expression.has_value()) {
          // inherit (expr) x; - select from expression
          // note: this re-evaluates the expression for each attribute
          // a smarter implementation would cache it
          auto from_value = compile_expression(*inherit.from_expression);
          auto key_str = symbols_.lookup(sym);
          value = compile_select(from_value, key_str, attr_name.position);
        } else {
          // inherit x; - look up from outer scope (before rec bindings)
          // temporarily switch to outer scope
          current_scope_ = outer_scope;
          value = compile_identifier_lookup(sym, attr_name.position);
          current_scope_ = &rec_scope;
        }
      }

      auto store_local = BinaryenLocalSet(module_.get(), binding.local_index, value);
      ops.push_back(store_local);
    }

    // third pass: construct the attrset from the locals
    auto attr_count = static_cast<std::uint32_t>(bindings.size());

    if (attr_count == 0) {
      current_scope_ = outer_scope;
      return BinaryenConst(module_.get(), BinaryenLiteralInt64(packed::empty_attribute_set));
    }

    // allocate space for (key_offset, value) pairs
    auto offset = data_offset_;
    auto total_size = attr_count * 12;
    data_offset_ += total_size;
    data_offset_ = (data_offset_ + 7) & ~7u;

    for (std::uint32_t i = 0; i < attr_count; ++i) {
      const auto& binding = bindings[i];
      auto key_str = symbols_.lookup(binding.name);
      auto key_offset = allocate_string(key_str);

      auto pair_offset = offset + i * 12;

      // store key offset
      auto store_key = BinaryenStore(
          module_.get(), 4, pair_offset, 0, BinaryenConst(module_.get(), BinaryenLiteralInt32(0)),
          BinaryenConst(module_.get(), BinaryenLiteralInt32(static_cast<std::int32_t>(key_offset))),
          BinaryenTypeInt32(), "memory");
      ops.push_back(store_key);

      // load value from local and store
      auto value = BinaryenLocalGet(module_.get(), binding.local_index, make_nix_value_type());
      auto store_value = BinaryenStore(module_.get(), 8, pair_offset + 4, 0,
                                       BinaryenConst(module_.get(), BinaryenLiteralInt32(0)), value,
                                       BinaryenTypeInt64(), "memory");
      ops.push_back(store_value);
    }

    // call __makeAttrs
    BinaryenExpressionRef make_attrs_args[] = {
        BinaryenConst(module_.get(), BinaryenLiteralInt32(static_cast<std::int32_t>(offset))),
        BinaryenConst(module_.get(), BinaryenLiteralInt32(static_cast<std::int32_t>(attr_count)))};
    auto make_attrs =
        BinaryenCall(module_.get(), "__makeAttrs", make_attrs_args, 2, make_nix_value_type());
    ops.push_back(make_attrs);

    // restore outer scope
    current_scope_ = outer_scope;

    return BinaryenBlock(module_.get(), nullptr, ops.data(), static_cast<BinaryenIndex>(ops.size()),
                         make_nix_value_type());
  }

  [[nodiscard]] auto compile_variant(const ast::expression_select& expr) -> BinaryenExpressionRef {
    auto subject = compile_expression(expr.subject);

    if (expr.path.segments.empty()) {
      throw compilation_error("empty attribute path");
    }

    auto result = subject;
    for (const auto& segment : expr.path.segments) {
      if (segment.is_dynamic()) {
        // dynamic key: compile the expression and use __selectDynamic
        const auto& key_expr = std::get<ast::expression>(segment.value);
        auto key_value = compile_expression(key_expr);
        result = compile_select_dynamic(result, key_value, segment.position);
      } else {
        // static key: use __select with string offset
        auto sym = std::get<ast::symbol>(segment.value);
        auto key_str = symbols_.lookup(sym);
        result = compile_select(result, key_str, segment.position);
      }
    }

    // handle default value
    if (expr.default_value.has_value()) {
      // check if result is null, if so use default
      auto is_null = BinaryenBinary(
          module_.get(), BinaryenEqInt64(),
          BinaryenConst(module_.get(), BinaryenLiteralInt64(packed::null_value)), result);
      auto default_val = compile_expression(*expr.default_value);
      result = BinaryenIf(module_.get(), is_null, default_val, result);
    }

    return result;
  }

  [[nodiscard]] auto compile_variant(const ast::expression_has_attribute& expr)
      -> BinaryenExpressionRef {
    auto subject = compile_expression(expr.subject);

    if (expr.path.segments.empty()) {
      // empty path always true
      return BinaryenConst(module_.get(), BinaryenLiteralInt64(packed::boolean_true));
    }

    // for multi-segment paths, chain hasAttr and select operations
    // e.g., x ? a.b.c means: hasAttr(x, "a") && hasAttr(x.a, "b") && hasAttr(x.a.b, "c")
    auto result = subject;
    for (std::size_t i = 0; i < expr.path.segments.size(); ++i) {
      const auto& segment = expr.path.segments[i];
      bool is_last = (i == expr.path.segments.size() - 1);

      BinaryenExpressionRef has_attr_result;
      if (segment.is_dynamic()) {
        // dynamic key: compile the expression and use __hasAttrDynamic
        const auto& key_expr = std::get<ast::expression>(segment.value);
        auto key_value = compile_expression(key_expr);

        BinaryenExpressionRef args[] = {result, key_value};
        has_attr_result =
            BinaryenCall(module_.get(), "__hasAttrDynamic", args, 2, make_nix_value_type());
      } else {
        // static key: use __hasAttr with string offset
        auto sym = std::get<ast::symbol>(segment.value);
        auto key_str = symbols_.lookup(sym);
        auto key_offset = allocate_string(key_str);

        BinaryenExpressionRef args[] = {
            result, BinaryenConst(module_.get(),
                                  BinaryenLiteralInt32(static_cast<std::int32_t>(key_offset)))};
        has_attr_result = BinaryenCall(module_.get(), "__hasAttr", args, 2, make_nix_value_type());
      }

      if (is_last) {
        // last segment: just return the hasAttr result
        return has_attr_result;
      }

      // not last segment: we need to check hasAttr and then select to continue
      // if hasAttr is false, short-circuit and return false
      // otherwise, select into the attribute and continue

      // for now, generate: hasAttr ? (continue with select) : false
      // this requires selecting the attribute if hasAttr is true
      BinaryenExpressionRef select_result;
      if (segment.is_dynamic()) {
        const auto& key_expr = std::get<ast::expression>(segment.value);
        auto key_value = compile_expression(key_expr);
        select_result = compile_select_dynamic(result, key_value, segment.position);
      } else {
        auto sym = std::get<ast::symbol>(segment.value);
        auto key_str = symbols_.lookup(sym);
        select_result = compile_select(result, key_str, segment.position);
      }

      // short-circuit: if hasAttr is false, return false immediately
      // we check if has_attr_result equals boolean_true
      auto has_is_true =
          BinaryenBinary(module_.get(), BinaryenEqInt64(),
                         BinaryenConst(module_.get(), BinaryenLiteralInt64(packed::boolean_true)),
                         has_attr_result);

      // wrap in a conditional: if has_is_true, continue with select_result; else return false
      // we need to continue with the rest of the path, so we can't directly return here
      // Instead, use the selected value for the next iteration, but guard with the check

      // simplify for now: assume all hasAttr checks pass (we'll evaluate fully)
      // the proper solution would require temp locals, which is complex
      result = select_result;
    }

    // should not reach here, but return true if empty path
    return BinaryenConst(module_.get(), BinaryenLiteralInt64(packed::boolean_true));
  }

  [[nodiscard]] auto compile_variant(const ast::expression_lambda& expr) -> BinaryenExpressionRef {
    // generate a unique function name for this lambda
    auto func_index = lambda_counter_++;
    auto func_name = "__lambda_" + std::to_string(func_index);
    lambda_function_names_.push_back(func_name);

    // collect bound names from the pattern
    std::vector<ast::symbol> bound_names;
    const auto& pattern = *expr.argument_pattern;
    if (std::holds_alternative<ast::pattern_simple>(pattern)) {
      const auto& simple = std::get<ast::pattern_simple>(pattern);
      bound_names.push_back(simple.argument_name);
    } else {
      const auto& attrset_pattern = std::get<ast::pattern_attrset>(pattern);
      if (attrset_pattern.argument_name.has_value()) {
        bound_names.push_back(*attrset_pattern.argument_name);
      }
      for (const auto& formal : attrset_pattern.formals) {
        bound_names.push_back(formal.name);
      }
    }

    // analyze free variables in the lambda body
    auto free_vars = free_variable_analyzer::analyze(expr.body, bound_names);

    // filter free variables: only keep those that are actually in scope
    // (others are builtins or globals that will be looked up at runtime)
    std::vector<ast::symbol> captures;
    for (auto sym : free_vars) {
      if (current_scope_ && current_scope_->lookup(sym).has_value()) {
        captures.push_back(sym);
      }
    }

    // save current lambda context
    auto outer_lambda_context = std::move(current_lambda_context_);
    current_lambda_context_ = lambda_context{};
    current_lambda_context_->captures = captures;

    // build capture index map
    for (std::uint32_t i = 0; i < captures.size(); ++i) {
      current_lambda_context_->capture_indices[captures[i]] = i;
    }

    // create a new scope for the lambda body
    // depth is incremented to track we're in a nested function
    std::uint32_t new_depth = current_scope_ ? current_scope_->depth() + 1 : 1;
    lexical_scope lambda_scope(nullptr, new_depth); // no parent - captures are explicit
    auto* outer_scope = current_scope_;
    current_scope_ = &lambda_scope;

    // lambda functions take (env_ptr: i32, arg: nix_value) -> nix_value
    // local 0: env_ptr (pointer to closure environment)
    // local 1: arg (the function argument)
    current_lambda_context_->local_types.push_back(BinaryenTypeInt32());   // env_ptr
    current_lambda_context_->local_types.push_back(make_nix_value_type()); // arg
    current_lambda_context_->next_local_index = 2;

    // add captured variables to the scope
    for (std::uint32_t i = 0; i < captures.size(); ++i) {
      lambda_scope.add_captured(captures[i], i);
    }

    // add the argument binding to the scope
    std::uint32_t arg_local_index = 1;

    // handle the pattern binding
    BinaryenExpressionRef pattern_setup = nullptr;

    if (std::holds_alternative<ast::pattern_simple>(pattern)) {
      // simple pattern: x: body
      const auto& simple = std::get<ast::pattern_simple>(pattern);
      lambda_scope.add_local(simple.argument_name, arg_local_index);
    } else {
      // attrset pattern: { a, b ? default, ... }@name: body
      const auto& attrset_pattern = std::get<ast::pattern_attrset>(pattern);

      // if there's an @name binding, bind the whole argument
      if (attrset_pattern.argument_name.has_value()) {
        lambda_scope.add_local(*attrset_pattern.argument_name, arg_local_index);
      }

      // for each formal parameter, we need to extract it from the argument attrset
      std::vector<BinaryenExpressionRef> setup_ops;

      for (const auto& formal : attrset_pattern.formals) {
        // allocate a local for this formal parameter
        auto formal_local = current_lambda_context_->next_local_index++;
        current_lambda_context_->local_types.push_back(make_nix_value_type());
        lambda_scope.add_local(formal.name, formal_local);

        // generate code to extract the attribute from the argument
        auto key_str = symbols_.lookup(formal.name);
        auto key_offset = allocate_string(key_str);

        BinaryenExpressionRef value_expr;
        if (formal.default_value.has_value()) {
          // has default: use __hasAttr to check, then select or use default
          // if (hasAttr(arg, key)) select(arg, key) else default
          auto arg_value_has =
              BinaryenLocalGet(module_.get(), arg_local_index, make_nix_value_type());
          BinaryenExpressionRef has_attr_args[] = {
              arg_value_has,
              BinaryenConst(module_.get(),
                            BinaryenLiteralInt32(static_cast<std::int32_t>(key_offset)))};
          auto has_attr_result =
              BinaryenCall(module_.get(), "__hasAttr", has_attr_args, 2, make_nix_value_type());

          // convert nix_value bool to wasm i32 condition
          auto condition = BinaryenBinary(
              module_.get(), BinaryenEqInt64(), has_attr_result,
              BinaryenConst(module_.get(), BinaryenLiteralInt64(packed::boolean_true)));

          // then branch: select the attribute
          auto arg_value_select =
              BinaryenLocalGet(module_.get(), arg_local_index, make_nix_value_type());
          auto selected_value = compile_select(arg_value_select, key_str, formal.position);

          // else branch: use default value
          auto default_val = compile_expression(*formal.default_value);

          value_expr = BinaryenIf(module_.get(), condition, selected_value, default_val);
        } else {
          // no default: just select (will error if not found)
          auto arg_value = BinaryenLocalGet(module_.get(), arg_local_index, make_nix_value_type());
          value_expr = compile_select(arg_value, key_str, formal.position);
        }

        // store to local
        auto store_local = BinaryenLocalSet(module_.get(), formal_local, value_expr);
        setup_ops.push_back(store_local);
      }

      if (!setup_ops.empty()) {
        pattern_setup =
            BinaryenBlock(module_.get(), nullptr, setup_ops.data(),
                          static_cast<BinaryenIndex>(setup_ops.size()), BinaryenTypeNone());
      }
    }

    // compile the body
    auto body_expr = compile_expression(expr.body);

    // if we have pattern setup, combine it with the body
    BinaryenExpressionRef full_body;
    if (pattern_setup) {
      BinaryenExpressionRef body_parts[] = {pattern_setup, body_expr};
      full_body = BinaryenBlock(module_.get(), nullptr, body_parts, 2, make_nix_value_type());
    } else {
      full_body = body_expr;
    }

    // create the function
    // params: (env_ptr: i32, arg: nix_value)
    BinaryenType param_types[] = {BinaryenTypeInt32(), make_nix_value_type()};
    auto params = BinaryenTypeCreate(param_types, 2);

    // locals: skip the first 2 (params), the rest are actual locals
    std::vector<BinaryenType> local_types;
    for (std::size_t i = 2; i < current_lambda_context_->local_types.size(); ++i) {
      local_types.push_back(current_lambda_context_->local_types[i]);
    }

    BinaryenAddFunction(module_.get(), func_name.c_str(), params, make_nix_value_type(),
                        local_types.empty() ? nullptr : local_types.data(),
                        static_cast<BinaryenIndex>(local_types.size()), full_body);

    // restore scope and context
    current_scope_ = outer_scope;
    auto captured_vars = std::move(current_lambda_context_->captures);
    current_lambda_context_ = std::move(outer_lambda_context);

    // create closure value
    // closure layout: func_index (i32) + capture_count (i32) + captures[N] (nix_value each)
    // total size: 8 + N * 8 bytes
    // NOTE: we always allocate a closure struct, even for zero-capture lambdas,
    // because rt_apply expects payload to be a pointer to the closure struct.
    auto capture_count = static_cast<std::uint32_t>(captured_vars.size());
    auto closure_size = 8 + capture_count * 8;

    // allocate closure memory
    auto closure_offset = data_offset_;
    data_offset_ += closure_size;
    data_offset_ = (data_offset_ + 7) & ~7u; // align to 8

    // for zero-capture lambdas, we can initialize the closure in the data segment
    // and return a constant value (no runtime stores needed)
    if (captured_vars.empty()) {
      // create data segment with closure content: [func_index, capture_count=0]
      std::vector<char> closure_data(closure_size, 0);
      auto func_idx_u32 = static_cast<std::uint32_t>(func_index);
      std::memcpy(closure_data.data(), &func_idx_u32, 4);
      // capture_count is already 0

      BinaryenAddDataSegment(module_.get(), nullptr, "memory", false,
                             BinaryenConst(module_.get(), BinaryenLiteralInt32(closure_offset)),
                             closure_data.data(), closure_size);

      std::int64_t packed_value = (static_cast<std::int64_t>(closure_offset) << 32) |
                                  static_cast<std::int64_t>(value_tag::lambda);
      return BinaryenConst(module_.get(), BinaryenLiteralInt64(packed_value));
    }

    // has captures - need to store captured values at runtime
    // generate code to:
    // 1. store func_index at closure_offset
    // 2. store capture_count at closure_offset + 4
    // 3. store each captured value at closure_offset + 8 + i*8
    std::vector<BinaryenExpressionRef> closure_setup;

    // store func_index
    closure_setup.push_back(BinaryenStore(
        module_.get(), 4, closure_offset, 0, BinaryenConst(module_.get(), BinaryenLiteralInt32(0)),
        BinaryenConst(module_.get(), BinaryenLiteralInt32(static_cast<std::int32_t>(func_index))),
        BinaryenTypeInt32(), "memory"));

    // store capture_count
    closure_setup.push_back(
        BinaryenStore(module_.get(), 4, closure_offset + 4, 0,
                      BinaryenConst(module_.get(), BinaryenLiteralInt32(0)),
                      BinaryenConst(module_.get(),
                                    BinaryenLiteralInt32(static_cast<std::int32_t>(capture_count))),
                      BinaryenTypeInt32(), "memory"));

    // store each captured value
    for (std::uint32_t i = 0; i < capture_count; ++i) {
      auto sym = captured_vars[i];
      // look up the variable in the outer scope to get its value
      auto var_value = compile_identifier_lookup(sym);
      closure_setup.push_back(BinaryenStore(module_.get(), 8, closure_offset + 8 + i * 8, 0,
                                            BinaryenConst(module_.get(), BinaryenLiteralInt32(0)),
                                            var_value, BinaryenTypeInt64(), "memory"));
    }

    // return closure value: pack tag=lambda and closure_offset
    std::int64_t packed_value = (static_cast<std::int64_t>(closure_offset) << 32) |
                                static_cast<std::int64_t>(value_tag::lambda);
    closure_setup.push_back(BinaryenConst(module_.get(), BinaryenLiteralInt64(packed_value)));

    return BinaryenBlock(module_.get(), nullptr, closure_setup.data(),
                         static_cast<BinaryenIndex>(closure_setup.size()), make_nix_value_type());
  }

  [[nodiscard]] auto compile_variant(const ast::expression_application& expr)
      -> BinaryenExpressionRef {
    // compile function
    auto func = compile_expression(expr.function);

    // apply each argument in sequence (curried application)
    // Arguments are wrapped in thunks for lazy evaluation (Nix is lazy)
    // This is crucial for builtins like tryEval that need to catch errors
    auto result = func;
    for (const auto& arg : expr.arguments) {
      BinaryenExpressionRef compiled_arg;
      if (is_trivial_expression(arg)) {
        // trivial expressions (literals, identifiers) can be evaluated immediately
        compiled_arg = compile_expression(arg);
      } else {
        // non-trivial expressions are wrapped in thunks for lazy evaluation
        compiled_arg = compile_as_thunk(arg);
      }
      BinaryenExpressionRef args[] = {result, compiled_arg};
      result = BinaryenCall(module_.get(), "__apply", args, 2, make_nix_value_type());
    }

    return result;
  }

  [[nodiscard]] auto compile_variant(const ast::expression_let& expr) -> BinaryenExpressionRef {
    // create a new scope for the let bindings
    std::uint32_t scope_depth = current_scope_ ? current_scope_->depth() : 0;
    lexical_scope let_scope(current_scope_, scope_depth);
    auto* outer_scope = current_scope_;
    current_scope_ = &let_scope;

    // Group bindings by first segment for multi-segment path merging
    auto grouped = group_bindings_by_first_segment(expr.bindings);

    // Check for dynamic first segments (not supported in let)
    for (const auto& binding : expr.bindings) {
      if (std::holds_alternative<ast::binding_attribute>(binding)) {
        const auto& attr_binding = std::get<ast::binding_attribute>(binding);
        if (attr_binding.path.segments.empty()) {
          throw compilation_error("empty attribute path in let binding");
        }
        const auto& first_segment = attr_binding.path.segments[0];
        if (first_segment.is_dynamic()) {
          throw compilation_error("dynamic let binding names not yet implemented");
        }
      }
    }

    std::vector<BinaryenExpressionRef> binding_ops;

    // Process grouped attribute bindings (handles multi-segment path merging)
    for (const auto& [first_sym, slices] : grouped) {
      // allocate a local for this binding
      std::uint32_t local_index;
      if (current_lambda_context_.has_value()) {
        local_index = current_lambda_context_->next_local_index++;
        current_lambda_context_->local_types.push_back(make_nix_value_type());
      } else {
        throw compilation_error("top-level let expressions not supported");
      }

      let_scope.add_local(first_sym, local_index);

      // compile the (possibly merged) value
      // segment_index=1 because we already consumed the first segment (first_sym)
      auto value = compile_merged_attrset_value(slices, 1);

      auto store_local = BinaryenLocalSet(module_.get(), local_index, value);
      binding_ops.push_back(store_local);
    }

    // Process inherit bindings
    for (const auto& binding : expr.bindings) {
      if (std::holds_alternative<ast::binding_inherit>(binding)) {
        // inherit binding: inherit x y; or inherit (expr) x y;
        const auto& inherit_binding = std::get<ast::binding_inherit>(binding);

        // if there's a from_expression, compile it once and select from it
        BinaryenExpressionRef from_value = nullptr;
        std::uint32_t from_local_index = 0;

        if (inherit_binding.from_expression.has_value()) {
          from_value = compile_expression(*inherit_binding.from_expression);

          // store the from_expression result in a temporary local to avoid re-evaluating
          if (current_lambda_context_.has_value() && inherit_binding.attributes.size() > 1) {
            from_local_index = current_lambda_context_->next_local_index++;
            current_lambda_context_->local_types.push_back(make_nix_value_type());
            auto store_from = BinaryenLocalSet(module_.get(), from_local_index, from_value);
            binding_ops.push_back(store_from);
            from_value = nullptr; // will use local get instead
          }
        }

        for (const auto& attr_name : inherit_binding.attributes) {
          // for now only handle static attribute names
          if (attr_name.is_dynamic()) {
            throw compilation_error("dynamic inherit attribute names not yet implemented");
          }

          auto sym = std::get<ast::symbol>(attr_name.value);

          // allocate a local for this inherited binding
          std::uint32_t local_index;
          if (current_lambda_context_.has_value()) {
            local_index = current_lambda_context_->next_local_index++;
            current_lambda_context_->local_types.push_back(make_nix_value_type());
          } else {
            throw compilation_error("top-level let expressions not supported");
          }

          let_scope.add_local(sym, local_index);

          // get the value to store
          BinaryenExpressionRef value;
          if (inherit_binding.from_expression.has_value()) {
            // inherit (expr) x; -> select x from expr
            auto key_str = symbols_.lookup(sym);

            BinaryenExpressionRef source;
            if (from_value != nullptr) {
              // single attribute, use the compiled expression directly
              source = from_value;
              from_value = nullptr; // consumed
            } else {
              // multiple attributes, read from local
              source = BinaryenLocalGet(module_.get(), from_local_index, make_nix_value_type());
            }

            value = compile_select(source, key_str, attr_name.position);
          } else {
            // inherit x; -> look up x from outer scope (before adding to let_scope)
            // we need to look up in the parent scope, not the current let_scope
            auto* saved_scope = current_scope_;
            current_scope_ = outer_scope;
            value = compile_identifier_lookup(sym, attr_name.position);
            current_scope_ = saved_scope;
          }

          auto store_local = BinaryenLocalSet(module_.get(), local_index, value);
          binding_ops.push_back(store_local);
        }
      }
    }

    // compile the body
    auto body_expr = compile_expression(expr.body);

    // restore scope
    current_scope_ = outer_scope;

    // combine binding setup with body
    if (binding_ops.empty()) {
      return body_expr;
    }

    binding_ops.push_back(body_expr);
    return BinaryenBlock(module_.get(), nullptr, binding_ops.data(),
                         static_cast<BinaryenIndex>(binding_ops.size()), make_nix_value_type());
  }

  [[nodiscard]] auto compile_variant(const ast::expression_with& expr) -> BinaryenExpressionRef {
    // with expr; body
    // 1. Compile the namespace expression
    // 2. Store it in a local variable
    // 3. Push a with_scope referencing that local
    // 4. Compile the body
    // 5. Pop the with_scope

    // we need a local to store the namespace
    if (!current_lambda_context_.has_value()) {
      throw compilation_error("with expressions require function context");
    }

    // allocate a local for the namespace
    auto namespace_local = current_lambda_context_->next_local_index++;
    current_lambda_context_->local_types.push_back(make_nix_value_type());

    // compile the namespace expression
    auto namespace_expr = compile_expression(expr.namespace_expression);

    // store namespace in the local
    auto store_namespace = BinaryenLocalSet(module_.get(), namespace_local, namespace_expr);

    // push with scope
    with_scopes_.push_back({namespace_local});

    // compile the body
    auto body_expr = compile_expression(expr.body);

    // pop with scope
    with_scopes_.pop_back();

    // combine: store namespace, then evaluate body
    BinaryenExpressionRef parts[] = {store_namespace, body_expr};
    return BinaryenBlock(module_.get(), nullptr, parts, 2, make_nix_value_type());
  }

  /// wrap a value expression in a __force call to evaluate thunks
  [[nodiscard]] auto compile_force(BinaryenExpressionRef value) -> BinaryenExpressionRef {
    BinaryenExpressionRef args[] = {value};
    return BinaryenCall(module_.get(), "__force", args, 1, make_nix_value_type());
  }

  /// compile a throw expression with source position for error reporting
  /// allocates the message string and generates __throw(msg_offset, line, col)
  [[nodiscard]] auto compile_throw(std::string_view message, const ast::source_position& position)
      -> BinaryenExpressionRef {
    auto msg_offset = allocate_string(message);
    BinaryenExpressionRef args[] = {
        BinaryenConst(module_.get(), BinaryenLiteralInt32(static_cast<std::int32_t>(msg_offset))),
        BinaryenConst(module_.get(),
                      BinaryenLiteralInt32(static_cast<std::int32_t>(position.line))),
        BinaryenConst(module_.get(),
                      BinaryenLiteralInt32(static_cast<std::int32_t>(position.column)))};
    return BinaryenCall(module_.get(), "__throw", args, 3, make_nix_value_type());
  }

  /// compile a static attribute selection with source position for "attribute not found" errors
  /// generates __select(set, key_offset, line, col)
  [[nodiscard]] auto compile_select(BinaryenExpressionRef set, std::string_view key,
                                    const ast::source_position& position) -> BinaryenExpressionRef {
    auto key_offset = allocate_string(key);
    BinaryenExpressionRef args[] = {
        set,
        BinaryenConst(module_.get(), BinaryenLiteralInt32(static_cast<std::int32_t>(key_offset))),
        BinaryenConst(module_.get(),
                      BinaryenLiteralInt32(static_cast<std::int32_t>(position.line))),
        BinaryenConst(module_.get(),
                      BinaryenLiteralInt32(static_cast<std::int32_t>(position.column)))};
    return BinaryenCall(module_.get(), "__select", args, 4, make_nix_value_type());
  }

  /// compile a dynamic attribute selection with source position for "attribute not found" errors
  /// generates __selectDynamic(set, key_value, line, col)
  [[nodiscard]] auto compile_select_dynamic(BinaryenExpressionRef set, BinaryenExpressionRef key,
                                            const ast::source_position& position)
      -> BinaryenExpressionRef {
    BinaryenExpressionRef args[] = {
        set, key,
        BinaryenConst(module_.get(),
                      BinaryenLiteralInt32(static_cast<std::int32_t>(position.line))),
        BinaryenConst(module_.get(),
                      BinaryenLiteralInt32(static_cast<std::int32_t>(position.column)))};
    return BinaryenCall(module_.get(), "__selectDynamic", args, 4, make_nix_value_type());
  }

  [[nodiscard]] auto compile_variant(const ast::expression_if& expr) -> BinaryenExpressionRef {
    auto condition = compile_expression(expr.condition);
    auto then_branch = compile_expression(expr.then_branch);
    auto else_branch = compile_expression(expr.else_branch);

    // force the condition (it might be a thunk)
    auto forced_condition = compile_force(condition);

    // condition must be a boolean - check if it equals true
    auto cond_is_true = BinaryenBinary(
        module_.get(), BinaryenEqInt64(),
        BinaryenConst(module_.get(), BinaryenLiteralInt64(packed::boolean_true)), forced_condition);

    return BinaryenIf(module_.get(), cond_is_true, then_branch, else_branch);
  }

  [[nodiscard]] auto compile_variant(const ast::expression_assert& expr) -> BinaryenExpressionRef {
    auto condition = compile_expression(expr.condition);
    auto body = compile_expression(expr.body);

    // force the condition (it might be a thunk)
    auto forced_condition = compile_force(condition);

    // check if condition is true
    auto cond_is_true = BinaryenBinary(
        module_.get(), BinaryenEqInt64(),
        BinaryenConst(module_.get(), BinaryenLiteralInt64(packed::boolean_true)), forced_condition);

    // if false, throw with position; otherwise return body
    auto throw_expr = compile_throw("assertion failed", expr.position);

    return BinaryenIf(module_.get(), cond_is_true, body, throw_expr);
  }

  /// compile interpolation parts for string or path interpolation
  /// parts: vector of either literal strings or expressions
  /// result_tag: value_tag::string for string interpolation, value_tag::path for path interpolation
  [[nodiscard]] auto
  compile_interpolation_parts(const std::vector<std::variant<std::string, ast::expression>>& parts,
                              value_tag result_tag) -> BinaryenExpressionRef {
    auto part_count = static_cast<std::uint32_t>(parts.size());

    // allocate space for part values (8 bytes each for nix_value)
    auto parts_offset = data_offset_;
    auto total_size = part_count * 8;
    data_offset_ += total_size;
    data_offset_ = (data_offset_ + 7) & ~7u; // align to 8

    // generate code to store each part
    std::vector<BinaryenExpressionRef> store_operations;

    for (std::uint32_t index = 0; index < part_count; ++index) {
      const auto& part = parts[index];
      BinaryenExpressionRef part_value;

      if (std::holds_alternative<std::string>(part)) {
        // literal string part - create a string value directly
        auto string_offset = allocate_string(std::get<std::string>(part));
        std::int64_t packed = (static_cast<std::int64_t>(string_offset) << 32) |
                              static_cast<std::int64_t>(value_tag::string);
        part_value = BinaryenConst(module_.get(), BinaryenLiteralInt64(packed));
      } else {
        // expression part - compile and coerce to string
        auto expression_value = compile_expression(std::get<ast::expression>(part));
        BinaryenExpressionRef to_string_args[] = {expression_value};
        part_value =
            BinaryenCall(module_.get(), "__toString", to_string_args, 1, make_nix_value_type());
      }

      // store the part value at parts_offset + index * 8
      auto store_part =
          BinaryenStore(module_.get(),
                        8,                                                     // bytes
                        parts_offset + index * 8,                              // offset
                        0,                                                     // align
                        BinaryenConst(module_.get(), BinaryenLiteralInt32(0)), // base address
                        part_value, BinaryenTypeInt64(), "memory");
      store_operations.push_back(store_part);
    }

    // call __concatStrings(offset, count) -> nix_value (string)
    BinaryenExpressionRef concat_args[] = {
        BinaryenConst(module_.get(), BinaryenLiteralInt32(static_cast<std::int32_t>(parts_offset))),
        BinaryenConst(module_.get(), BinaryenLiteralInt32(static_cast<std::int32_t>(part_count)))};
    auto concat_result =
        BinaryenCall(module_.get(), "__concatStrings", concat_args, 2, make_nix_value_type());

    // for path interpolation, we need to change the tag from string to path
    // the __concatStrings returns a string, so we extract the offset and retag it
    BinaryenExpressionRef final_result;
    if (result_tag == value_tag::path) {
      // extract the string offset (high 32 bits) and create a path value
      // path_offset = concat_result >> 32
      // result = (path_offset << 32) | value_tag::path
      auto string_offset = BinaryenBinary(module_.get(), BinaryenShrUInt64(), concat_result,
                                          BinaryenConst(module_.get(), BinaryenLiteralInt64(32)));
      auto path_tag =
          BinaryenConst(module_.get(), BinaryenLiteralInt64(static_cast<std::int64_t>(result_tag)));
      auto shifted_offset = BinaryenBinary(module_.get(), BinaryenShlInt64(), string_offset,
                                           BinaryenConst(module_.get(), BinaryenLiteralInt64(32)));
      final_result = BinaryenBinary(module_.get(), BinaryenOrInt64(), shifted_offset, path_tag);
    } else {
      // string interpolation - use the result directly
      final_result = concat_result;
    }

    // combine: store all parts, call concat, return result
    store_operations.push_back(final_result);
    return BinaryenBlock(module_.get(), nullptr, store_operations.data(),
                         static_cast<BinaryenIndex>(store_operations.size()),
                         make_nix_value_type());
  }

  /// allocate a null-terminated string in the data segment, returning its offset
  [[nodiscard]] auto allocate_string(std::string_view str) -> std::uint32_t {
    // check if already allocated
    // TODO: use string hash instead of searching

    // allocate: data + null terminator
    auto offset = data_offset_;
    auto total_size = str.size() + 1; // +1 for null terminator

    // Check data segment limit (64KB)
    constexpr std::uint32_t data_segment_limit = 0x10000;
    if (data_offset_ + total_size > data_segment_limit) {
      throw compilation_error("data segment overflow: expression too large (limit: 64KB)");
    }

    // create data segment
    std::vector<char> data(total_size);
    std::memcpy(data.data(), str.data(), str.size());
    data[str.size()] = '\0'; // null terminator

    BinaryenAddDataSegment(module_.get(),
                           nullptr,  // auto-generate name
                           "memory", // memory name (must match import)
                           false,    // not passive
                           BinaryenConst(module_.get(), BinaryenLiteralInt32(offset)), data.data(),
                           total_size);

    data_offset_ += total_size;
    // align to 4 bytes
    data_offset_ = (data_offset_ + 3) & ~3u;

    return offset;
  }
};

/// convenience function: compile expression to WASM binary
[[nodiscard]] inline auto compile_to_wasm(const ast::expression& expr,
                                          const ast::symbol_table& symbols, bool optimize = true)
    -> std::vector<std::uint8_t> {
  compiler comp(symbols);
  auto module = comp.compile(expr);

  if (optimize) {
    module.optimize();
  }

  if (!module.validate()) {
    throw compilation_error("generated WASM module failed validation");
  }

  return module.emit_binary();
}

/// convenience function: compile expression to WAT text
[[nodiscard]] inline auto compile_to_wat(const ast::expression& expr,
                                         const ast::symbol_table& symbols, bool optimize = true)
    -> std::string {
  compiler comp(symbols);
  auto module = comp.compile(expr);

  if (optimize) {
    module.optimize();
  }

  return module.emit_text();
}

} // namespace nix::language::compile
