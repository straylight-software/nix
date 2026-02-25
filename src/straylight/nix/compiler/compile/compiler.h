#pragma once
///@file straylight/nix/compiler/compile/compiler.h
/// Compiles Nix AST to WebAssembly using binaryen.

#include <algorithm>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <binaryen-c.h>

#include "straylight/nix/compiler/ast/expression.h"
#include "straylight/nix/compiler/ast/symbol_table.h"
#include "straylight/nix/compiler/compile/capture.h"
#include "straylight/nix/compiler/compile/wasm_types.h"

namespace straylight::nix::compiler::compile {

/// hash function for ast::symbol to use in unordered containers
struct symbol_hash {
  auto operator()(ast::symbol s) const noexcept -> std::size_t {
    return std::hash<std::uint32_t>{}(s.index_);
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
    std::visit([this](const auto& e) { visit_variant(e); }, expr->data_);
  }

  void visit_variant(const ast::expression_identifier& expr) {
    if (bound_.find(expr.name_) == bound_.end()) {
      free_.insert(expr.name_);
    }
  }

  void visit_variant(const ast::expression_integer&) {}
  void visit_variant(const ast::expression_float&) {}
  void visit_variant(const ast::expression_string&) {}
  void visit_variant(const ast::expression_path&) {}

  void visit_variant(const ast::expression_string_interpolated& expr) {
    for (const auto& part : expr.parts_) {
      if (std::holds_alternative<ast::expression>(part)) {
        visit(std::get<ast::expression>(part));
      }
    }
  }

  void visit_variant(const ast::expression_path_interpolated& expr) {
    for (const auto& part : expr.parts_) {
      if (std::holds_alternative<ast::expression>(part)) {
        visit(std::get<ast::expression>(part));
      }
    }
  }

  void visit_variant(const ast::expression_binary_operation& expr) {
    visit(expr.left_);
    visit(expr.right_);
  }

  void visit_variant(const ast::expression_unary_operation& expr) { visit(expr.operand_); }

  void visit_variant(const ast::expression_list& expr) {
    for (const auto& element : expr.elements_) {
      visit(element);
    }
  }

  void visit_variant(const ast::expression_attribute_set& expr) {
    // for rec sets, all binding names are in scope for all values
    std::vector<ast::symbol> new_bindings;
    if (expr.is_recursive_) {
      for (const auto& binding : expr.bindings_) {
        if (std::holds_alternative<ast::binding_attribute>(binding)) {
          const auto& attr_binding = std::get<ast::binding_attribute>(binding);
          if (attr_binding.path_.segments_.size() == 1 &&
              !attr_binding.path_.segments_[0].is_dynamic()) {
            auto sym = std::get<ast::symbol>(attr_binding.path_.segments_[0].value_);
            new_bindings.push_back(sym);
            bound_.insert(sym);
          }
        }
      }
    }

    for (const auto& binding : expr.bindings_) {
      if (std::holds_alternative<ast::binding_attribute>(binding)) {
        const auto& attr_binding = std::get<ast::binding_attribute>(binding);
        visit(attr_binding.value_);
        // visit dynamic path segments
        for (const auto& segment : attr_binding.path_.segments_) {
          if (segment.is_dynamic()) {
            visit(std::get<ast::expression>(segment.value_));
          }
        }
      } else {
        const auto& inherit = std::get<ast::binding_inherit>(binding);
        if (inherit.from_expression_.has_value()) {
          visit(*inherit.from_expression_);
        } else {
          // inherit x; pulls x from outer scope
          for (const auto& attr : inherit.attributes_) {
            if (!attr.is_dynamic() && std::holds_alternative<ast::symbol>(attr.value_)) {
              auto name = std::get<ast::symbol>(attr.value_);
              if (bound_.find(name) == bound_.end()) {
                free_.insert(name);
              }
            }
          }
        }
      }
    }

    // remove recursive bindings (restore scope)
    if (expr.is_recursive_) {
      for (auto sym : new_bindings) {
        bound_.erase(sym);
      }
    }
  }

  void visit_variant(const ast::expression_select& expr) {
    visit(expr.subject_);
    // visit dynamic path segments
    for (const auto& segment : expr.path_.segments_) {
      if (segment.is_dynamic()) {
        visit(std::get<ast::expression>(segment.value_));
      }
    }
    if (expr.default_value_.has_value()) {
      visit(*expr.default_value_);
    }
  }

  void visit_variant(const ast::expression_has_attribute& expr) {
    visit(expr.subject_);
    for (const auto& segment : expr.path_.segments_) {
      if (segment.is_dynamic()) {
        visit(std::get<ast::expression>(segment.value_));
      }
    }
  }

  void visit_variant(const ast::expression_lambda& expr) {
    // lambda introduces new bindings
    std::vector<ast::symbol> lambda_bindings;

    const auto& pattern = *expr.argument_pattern_;
    if (std::holds_alternative<ast::pattern_simple>(pattern)) {
      const auto& simple = std::get<ast::pattern_simple>(pattern);
      lambda_bindings.push_back(simple.argument_name_);
    } else {
      const auto& attrset_pattern = std::get<ast::pattern_attrset>(pattern);
      if (attrset_pattern.argument_name_.has_value()) {
        lambda_bindings.push_back(*attrset_pattern.argument_name_);
      }
      for (const auto& formal : attrset_pattern.formals_) {
        lambda_bindings.push_back(formal.name_);
        // default values are evaluated in outer scope
        if (formal.default_value_.has_value()) {
          visit(*formal.default_value_);
        }
      }
    }

    // add lambda bindings to scope
    for (auto sym : lambda_bindings) {
      bound_.insert(sym);
    }

    // visit body
    visit(expr.body_);

    // remove lambda bindings (restore scope)
    for (auto sym : lambda_bindings) {
      bound_.erase(sym);
    }
  }

  void visit_variant(const ast::expression_application& expr) {
    visit(expr.function_);
    for (const auto& arg : expr.arguments_) {
      visit(arg);
    }
  }

  void visit_variant(const ast::expression_let& expr) {
    // let bindings are mutually recursive
    std::vector<ast::symbol> let_bindings;
    for (const auto& binding : expr.bindings_) {
      if (std::holds_alternative<ast::binding_attribute>(binding)) {
        const auto& attr_binding = std::get<ast::binding_attribute>(binding);
        if (attr_binding.path_.segments_.size() == 1 &&
            !attr_binding.path_.segments_[0].is_dynamic()) {
          auto sym = std::get<ast::symbol>(attr_binding.path_.segments_[0].value_);
          let_bindings.push_back(sym);
          bound_.insert(sym);
        }
      }
    }

    // visit binding values (in scope of all let bindings)
    for (const auto& binding : expr.bindings_) {
      if (std::holds_alternative<ast::binding_attribute>(binding)) {
        const auto& attr_binding = std::get<ast::binding_attribute>(binding);
        visit(attr_binding.value_);
      } else {
        const auto& inherit = std::get<ast::binding_inherit>(binding);
        if (inherit.from_expression_.has_value()) {
          visit(*inherit.from_expression_);
        } else {
          for (const auto& attr : inherit.attributes_) {
            if (!attr.is_dynamic() && std::holds_alternative<ast::symbol>(attr.value_)) {
              auto name = std::get<ast::symbol>(attr.value_);
              if (bound_.find(name) == bound_.end()) {
                free_.insert(name);
              }
            }
          }
        }
      }
    }

    // visit body
    visit(expr.body_);

    // restore scope
    for (auto sym : let_bindings) {
      bound_.erase(sym);
    }
  }

  void visit_variant(const ast::expression_with& expr) {
    visit(expr.namespace_expression_);
    // with introduces dynamic scope, we can't statically know what's bound
    // conservatively, we visit the body without adding bindings
    // (actual free variable detection for `with` is conservative)
    visit(expr.body_);
  }

  void visit_variant(const ast::expression_if& expr) {
    visit(expr.condition_);
    visit(expr.then_branch_);
    visit(expr.else_branch_);
  }

  void visit_variant(const ast::expression_assert& expr) {
    visit(expr.condition_);
    visit(expr.body_);
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

  /// Construct with a data segment base offset.
  /// Used when compiling imported modules to avoid data segment collisions.
  /// Each module's data segment will start at base_offset.
  compiler(const ast::symbol_table& symbols, std::uint32_t data_segment_base)
      : symbols_(symbols), data_offset_(data_segment_base) {
    setup_module();
  }

  /// Get the final data segment offset (high water mark).
  /// Use this to determine the base offset for the next module.
  [[nodiscard]] auto data_segment_end() const noexcept -> std::uint32_t { return data_offset_; }

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

  // For rec attrset thunks: maps symbol values to their memory offset in the rec environment.
  // When compiling a rec thunk body, rec-scope variables read from these offsets instead of
  // being captured. This enables mutual references.
  std::unordered_map<std::uint32_t, std::uint32_t> current_rec_binding_offsets_;

  // For let bindings: maps symbol index to their memory offset.
  // When compiling values in a let scope, let-bound variables read from these offsets
  // instead of being captured. This enables recursive let bindings (let f = x: f ...).
  std::unordered_map<std::uint32_t, std::uint32_t> current_let_binding_offsets_;

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

    // expect boolean (force + type check): (nix_value, line: i32, col: i32) -> nix_value
    // Throws type_error if value is not a boolean
    BinaryenType expect_bool_params[] = {nix_value_type, BinaryenTypeInt32(), BinaryenTypeInt32()};
    BinaryenAddFunctionImport(module_.get(), "__expectBool", "runtime", "__expectBool",
                              BinaryenTypeCreate(expect_bool_params, 3), nix_value_type);

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
    return std::visit([this](const auto& e) { return compile_variant(e); }, expr->data_);
  }

  /// compile an expression as a thunk (lazy evaluation)
  /// This creates a function that evaluates the expression when called,
  /// capturing any free variables from the current scope.
  /// Returns a nix_value with tag=thunk.
  /// @param policy Controls whether captured values are forced at capture time.
  ///        - force_eager (default): force captures at thunk creation (non-recursive contexts)
  ///        - preserve_lazy: store captures without forcing (recursive contexts like let/rec)
  [[nodiscard]] auto compile_as_thunk(const ast::expression& expr,
                                      capture_policy policy = capture_policy::force_eager)
      -> BinaryenExpressionRef {
    // generate a unique function name for this thunk
    auto func_index = thunk_counter_++;
    auto func_name = "__thunk_" + std::to_string(func_index);
    thunk_function_names_.push_back(func_name);

    // analyze free variables in the expression
    std::vector<ast::symbol> bound_names; // thunks have no bound parameters
    auto free_vars = free_variable_analyzer::analyze(expr, bound_names);

    // filter free variables: only keep those that are actually in scope
    // Also exclude let-bound and rec-bound variables - they're accessed via shared memory
    std::vector<ast::symbol> captures;
    for (auto sym : free_vars) {
      // Skip variables in let scope (accessed via shared memory)
      if (current_let_binding_offsets_.count(sym.index_) > 0) {
        continue;
      }
      // Skip variables in rec scope (accessed via shared memory)
      if (current_rec_binding_offsets_.count(sym.index_) > 0) {
        continue;
      }
      if (current_scope_ && current_scope_->lookup(sym).has_value()) {
        captures.push_back(sym);
      }
    }

    // save current lambda context
    auto outer_lambda_context = std::move(current_lambda_context_);
    current_lambda_context_ = lambda_context{};
    current_lambda_context_->captures = captures;

    // build capture index map
    for (std::uint32_t idx = 0; idx < captures.size(); ++idx) {
      current_lambda_context_->capture_indices[captures[idx]] = idx;
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
    for (std::uint32_t idx = 0; idx < captures.size(); ++idx) {
      thunk_scope.add_captured(captures[idx], idx);
    }

    // compile the body
    auto body_expr = compile_expression(expr);

    // create the function
    // params: (env_ptr: i32)
    BinaryenType param_types[] = {BinaryenTypeInt32()};
    auto params = BinaryenTypeCreate(param_types, 1);

    // locals: skip the first 1 (env_ptr param), the rest are actual locals
    std::vector<BinaryenType> local_types;
    for (std::size_t idx = 1; idx < current_lambda_context_->local_types.size(); ++idx) {
      local_types.push_back(current_lambda_context_->local_types[idx]);
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
    for (std::uint32_t idx = 0; idx < capture_count; ++idx) {
      auto sym = captured_vars[idx];
      // look up the variable in the outer scope to get its value
      // For rec attrsets, we capture without forcing so mutual references work
      auto var_value = compile_identifier_lookup(sym, {0, 0, 0}, should_force_captures(policy));
      thunk_setup.push_back(BinaryenStore(module_.get(), 8, env_offset + 4 + idx * 8, 0,
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

  /// Compile `inherit (from_expr) attr;` as a thunk for lazy evaluation.
  /// The thunk evaluates from_expr and selects attr from it when forced.
  /// This is crucial for fixpoint patterns like:
  ///   makeExtensible (self: { trivial = {...}; inherit (self.trivial) id; })
  [[nodiscard]] auto compile_inherit_from_as_thunk(const ast::expression& from_expr,
                                                   std::string_view attr,
                                                   const ast::source_position& position)
      -> BinaryenExpressionRef {
    // Generate thunk function
    auto func_index = thunk_counter_++;
    auto func_name = "__thunk_" + std::to_string(func_index);
    thunk_function_names_.push_back(func_name);

    // Analyze free variables in from_expr
    std::vector<ast::symbol> bound_names;
    auto free_vars = free_variable_analyzer::analyze(from_expr, bound_names);

    // Filter to variables in scope (excluding let/rec bindings which use shared memory)
    std::vector<ast::symbol> captures;
    for (auto sym : free_vars) {
      if (current_let_binding_offsets_.count(sym.index_) > 0) {
        continue;
      }
      if (current_rec_binding_offsets_.count(sym.index_) > 0) {
        continue;
      }
      if (current_scope_ && current_scope_->lookup(sym).has_value()) {
        captures.push_back(sym);
      }
    }

    // Save context
    auto outer_lambda_context = std::move(current_lambda_context_);
    current_lambda_context_ = lambda_context{};
    current_lambda_context_->captures = captures;

    // Build capture index map
    for (std::uint32_t idx = 0; idx < captures.size(); ++idx) {
      current_lambda_context_->capture_indices[captures[idx]] = idx;
    }

    // Create thunk scope
    std::uint32_t new_depth = current_scope_ ? current_scope_->depth() + 1 : 1;
    lexical_scope thunk_scope(nullptr, new_depth);
    auto* outer_scope = current_scope_;
    current_scope_ = &thunk_scope;

    // Thunk params: (env_ptr: i32) -> nix_value
    current_lambda_context_->local_types.push_back(BinaryenTypeInt32());
    current_lambda_context_->next_local_index = 1;

    // Add captures to scope
    for (std::uint32_t idx = 0; idx < captures.size(); ++idx) {
      thunk_scope.add_captured(captures[idx], idx);
    }

    // Compile: from_expr.attr
    auto from_value = compile_expression(from_expr);
    auto thunk_body = compile_select(from_value, attr, position);

    // Create the function
    BinaryenType param_types[] = {BinaryenTypeInt32()};
    auto params = BinaryenTypeCreate(param_types, 1);

    std::vector<BinaryenType> local_types;
    for (std::size_t idx = 1; idx < current_lambda_context_->local_types.size(); ++idx) {
      local_types.push_back(current_lambda_context_->local_types[idx]);
    }

    BinaryenAddFunction(module_.get(), func_name.c_str(), params, make_nix_value_type(),
                        local_types.empty() ? nullptr : local_types.data(),
                        static_cast<BinaryenIndex>(local_types.size()), thunk_body);

    // Restore context
    current_scope_ = outer_scope;
    auto captured_vars = std::move(current_lambda_context_->captures);
    current_lambda_context_ = std::move(outer_lambda_context);

    // Create thunk value
    if (captured_vars.empty()) {
      BinaryenExpressionRef make_thunk_args[] = {
          BinaryenConst(module_.get(), BinaryenLiteralInt32(static_cast<std::int32_t>(func_index))),
          BinaryenConst(module_.get(), BinaryenLiteralInt32(0)),
          BinaryenConst(module_.get(), BinaryenLiteralInt32(0))};
      return BinaryenCall(module_.get(), "__makeThunk", make_thunk_args, 3, make_nix_value_type());
    }

    // Allocate environment for captures
    auto capture_count = static_cast<std::uint32_t>(captured_vars.size());
    auto env_size = 4 + capture_count * 8;
    auto env_offset = data_offset_;
    data_offset_ += env_size;
    data_offset_ = (data_offset_ + 7) & ~7u;

    std::vector<BinaryenExpressionRef> thunk_setup;

    // Store capture count
    thunk_setup.push_back(BinaryenStore(
        module_.get(), 4, env_offset, 0, BinaryenConst(module_.get(), BinaryenLiteralInt32(0)),
        BinaryenConst(module_.get(),
                      BinaryenLiteralInt32(static_cast<std::int32_t>(capture_count))),
        BinaryenTypeInt32(), "memory"));

    // Store captured values using preserve_lazy semantics (crucial for fixpoints)
    for (std::uint32_t idx = 0; idx < capture_count; ++idx) {
      auto sym = captured_vars[idx];
      auto var_value = compile_identifier_lookup(
          sym, {0, 0, 0}, should_force_captures(capture_policy::preserve_lazy));
      thunk_setup.push_back(BinaryenStore(module_.get(), 8, env_offset + 4 + idx * 8, 0,
                                          BinaryenConst(module_.get(), BinaryenLiteralInt32(0)),
                                          var_value, BinaryenTypeInt64(), "memory"));
    }

    // Create thunk
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

  /// compile an expression as a thunk for recursive attrset bindings
  /// The thunk reads rec-scope variables from shared memory at force-time, not capture-time.
  /// This enables mutual references like rec { x = y + 1; y = 1; }.
  [[nodiscard]] auto
  compile_rec_thunk(const ast::expression& expr, std::uint32_t rec_env_offset,
                    const std::unordered_map<std::uint32_t, std::uint32_t>& rec_binding_offsets)
      -> BinaryenExpressionRef {
    // generate a unique function name for this thunk
    auto func_index = thunk_counter_++;
    auto func_name = "__thunk_" + std::to_string(func_index);
    thunk_function_names_.push_back(func_name);

    // analyze free variables in the expression
    std::vector<ast::symbol> bound_names;
    auto free_vars = free_variable_analyzer::analyze(expr, bound_names);

    // separate free variables into:
    // 1. rec-scope variables (read from shared memory at force-time)
    // 2. other variables (captured normally)
    std::vector<ast::symbol> captures;
    std::vector<ast::symbol> rec_vars;
    for (auto sym : free_vars) {
      if (rec_binding_offsets.count(sym.index_)) {
        rec_vars.push_back(sym);
      } else if (current_scope_ && current_scope_->lookup(sym).has_value()) {
        captures.push_back(sym);
      }
    }

    // save current lambda context
    auto outer_lambda_context = std::move(current_lambda_context_);
    current_lambda_context_ = lambda_context{};
    current_lambda_context_->captures = captures;

    // build capture index map (only for non-rec captures)
    for (std::uint32_t idx = 0; idx < captures.size(); ++idx) {
      current_lambda_context_->capture_indices[captures[idx]] = idx;
    }

    // create a new scope for the thunk body
    std::uint32_t new_depth = current_scope_ ? current_scope_->depth() + 1 : 1;
    lexical_scope thunk_scope(nullptr, new_depth);
    auto* outer_scope = current_scope_;
    current_scope_ = &thunk_scope;

    // thunk function takes (env_ptr: i32) -> nix_value
    // env layout: capture_count (i32), captures[], rec_env_offset (i32)
    current_lambda_context_->local_types.push_back(BinaryenTypeInt32()); // env_ptr
    current_lambda_context_->next_local_index = 1;

    // add captured variables to scope
    for (std::uint32_t idx = 0; idx < captures.size(); ++idx) {
      thunk_scope.add_captured(captures[idx], idx);
    }

    // For rec-scope variables, we'll generate code that reads from shared memory
    // Store the rec_binding_offsets in a member so compile_identifier_lookup can use it
    auto outer_rec_offsets = std::move(current_rec_binding_offsets_);
    current_rec_binding_offsets_ = rec_binding_offsets;

    // compile the body
    auto body_expr = compile_expression(expr);

    // restore rec offsets
    current_rec_binding_offsets_ = std::move(outer_rec_offsets);

    // create the function
    BinaryenType param_types[] = {BinaryenTypeInt32()};
    auto params = BinaryenTypeCreate(param_types, 1);

    std::vector<BinaryenType> local_types;
    for (std::size_t idx = 1; idx < current_lambda_context_->local_types.size(); ++idx) {
      local_types.push_back(current_lambda_context_->local_types[idx]);
    }

    BinaryenAddFunction(module_.get(), func_name.c_str(), params, make_nix_value_type(),
                        local_types.empty() ? nullptr : local_types.data(),
                        static_cast<BinaryenIndex>(local_types.size()), body_expr);

    // restore scope and context
    current_scope_ = outer_scope;
    auto captured_vars = std::move(current_lambda_context_->captures);
    current_lambda_context_ = std::move(outer_lambda_context);

    // Build thunk environment:
    // Layout: capture_count (i32), captures[], rec_env_offset (i32)
    auto capture_count = static_cast<std::uint32_t>(captured_vars.size());
    // +4 for capture_count, +4 for rec_env_offset
    auto env_size = 4 + capture_count * 8 + 4;

    auto env_offset = data_offset_;
    data_offset_ += env_size;
    data_offset_ = (data_offset_ + 7) & ~7u;

    std::vector<BinaryenExpressionRef> thunk_setup;

    // store capture_count
    thunk_setup.push_back(BinaryenStore(
        module_.get(), 4, env_offset, 0, BinaryenConst(module_.get(), BinaryenLiteralInt32(0)),
        BinaryenConst(module_.get(),
                      BinaryenLiteralInt32(static_cast<std::int32_t>(capture_count))),
        BinaryenTypeInt32(), "memory"));

    // store captured values (from outer scope, not rec scope)
    for (std::uint32_t idx = 0; idx < capture_count; ++idx) {
      auto sym = captured_vars[idx];
      auto var_value = compile_identifier_lookup(sym);
      thunk_setup.push_back(BinaryenStore(module_.get(), 8, env_offset + 4 + idx * 8, 0,
                                          BinaryenConst(module_.get(), BinaryenLiteralInt32(0)),
                                          var_value, BinaryenTypeInt64(), "memory"));
    }

    // store rec_env_offset at the end
    thunk_setup.push_back(BinaryenStore(
        module_.get(), 4, env_offset + 4 + capture_count * 8, 0,
        BinaryenConst(module_.get(), BinaryenLiteralInt32(0)),
        BinaryenConst(module_.get(),
                      BinaryenLiteralInt32(static_cast<std::int32_t>(rec_env_offset))),
        BinaryenTypeInt32(), "memory"));

    // call __makeThunk
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
    std::int64_t packed = (static_cast<std::int64_t>(expr.value_) << 32) |
                          static_cast<std::int64_t>(value_tag::integer);
    return BinaryenConst(module_.get(), BinaryenLiteralInt64(packed));
  }

  /// compile float literal
  [[nodiscard]] auto compile_variant(const ast::expression_float& expr) -> BinaryenExpressionRef {
    // floats are stored as 64-bit IEEE 754 doubles
    // we reinterpret the double bits as the payload
    std::uint64_t bits;
    std::memcpy(&bits, &expr.value_, sizeof(bits));

    // pack tag=3 (float) and the double bits
    // since we need full 64 bits for the double, we store it in a data segment
    // and return a pointer to it
    auto offset = data_offset_;
    std::vector<char> data(8);
    std::memcpy(data.data(), &expr.value_, 8);

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
    auto offset = allocate_string(expr.value_);

    // pack tag=4 (string) and offset into i64
    std::int64_t packed =
        (static_cast<std::int64_t>(offset) << 32) | static_cast<std::int64_t>(value_tag::string);
    return BinaryenConst(module_.get(), BinaryenLiteralInt64(packed));
  }

  /// compile identifier reference
  [[nodiscard]] auto compile_variant(const ast::expression_identifier& expr)
      -> BinaryenExpressionRef {
    return compile_identifier_lookup(expr.name_, expr.position_);
  }

  /// compile identifier lookup - shared between direct identifier references and closure capture
  /// position is used for error reporting when looking up in with scopes
  /// force_value: if true (default), forces the value (for normal access); if false, returns raw
  /// value
  [[nodiscard]] auto compile_identifier_lookup(ast::symbol name,
                                               ast::source_position position = {0, 0, 0},
                                               bool force_value = true) -> BinaryenExpressionRef {
    // Check if this is a rec-scope variable (when compiling rec thunk bodies)
    // These are read from shared memory at force-time, not captured
    auto rec_it = current_rec_binding_offsets_.find(name.index_);
    if (rec_it != current_rec_binding_offsets_.end()) {
      // Read from the rec environment in memory
      auto mem_offset = rec_it->second;
      auto value = BinaryenLoad(module_.get(), 8, 0, mem_offset, 0, BinaryenTypeInt64(),
                                BinaryenConst(module_.get(), BinaryenLiteralInt32(0)), "memory");
      // Force the value (it's a thunk stored in the rec environment)
      return compile_force(value);
    }

    // Check if this is a let-scope variable (for recursive let bindings)
    // These are read from shared memory to enable self-references in closures
    auto let_it = current_let_binding_offsets_.find(name.index_);
    if (let_it != current_let_binding_offsets_.end()) {
      // Read from the let environment in memory
      auto mem_offset = let_it->second;
      auto value = BinaryenLoad(module_.get(), 8, 0, mem_offset, 0, BinaryenTypeInt64(),
                                BinaryenConst(module_.get(), BinaryenLiteralInt32(0)), "memory");
      // Force the value in case it's a thunk
      return force_value ? compile_force(value) : value;
    }

    // first check the current scope hierarchy for local variables
    if (current_scope_) {
      auto lookup_result = current_scope_->lookup(name);
      if (lookup_result.has_value()) {
        const auto& local_binding = lookup_result->first;
        if (local_binding.location == variable_location::local) {
          // local variable - read from WASM local
          auto local_value =
              BinaryenLocalGet(module_.get(), local_binding.local_index, make_nix_value_type());
          // Force the value unless explicitly asked not to (for rec attrset captures)
          return force_value ? compile_force(local_value) : local_value;
        } else {
          // captured variable - read from closure environment
          // env_ptr is local 0 and already points to the captures area (closure_ptr + 8)
          // captures are at env_ptr + capture_index * 8
          auto env_ptr = BinaryenLocalGet(module_.get(), 0, BinaryenTypeInt32());
          auto capture_offset = local_binding.capture_index * 8;
          auto captured_value = BinaryenLoad(module_.get(), 8, 0, capture_offset, 0,
                                             BinaryenTypeInt64(), env_ptr, "memory");
          // Force captured variables unless asked not to
          return force_value ? compile_force(captured_value) : captured_value;
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

      // iterate from outermost to innermost to build nested if-else
      // The last processed (innermost) becomes the outermost `if` condition,
      // so it gets checked first. This ensures inner `with` shadows outer.
      for (auto it = with_scopes_.begin(); it != with_scopes_.end(); ++it) {
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
    if (expr.op_ == ast::binary_operator::logical_and) {
      return compile_logical_and(expr);
    }
    if (expr.op_ == ast::binary_operator::logical_or) {
      return compile_logical_or(expr);
    }
    if (expr.op_ == ast::binary_operator::logical_implies) {
      return compile_logical_implies(expr);
    }
    if (expr.op_ == ast::binary_operator::pipe_right) {
      // a |> b is equivalent to b a
      return compile_pipe_right(expr);
    }
    if (expr.op_ == ast::binary_operator::pipe_left) {
      // a <| b is equivalent to a b
      return compile_pipe_left(expr);
    }

    auto left = compile_expression(expr.left_);
    auto right = compile_expression(expr.right_);

    // select the appropriate builtin
    const char* builtin = nullptr;
    switch (expr.op_) {
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
        (expr.op_ == ast::binary_operator::add || expr.op_ == ast::binary_operator::subtract ||
         expr.op_ == ast::binary_operator::multiply || expr.op_ == ast::binary_operator::divide);
    if (is_arithmetic) {
      BinaryenExpressionRef args[] = {
          left, right,
          BinaryenConst(module_.get(),
                        BinaryenLiteralInt32(static_cast<std::int32_t>(expr.position_.line_))),
          BinaryenConst(module_.get(),
                        BinaryenLiteralInt32(static_cast<std::int32_t>(expr.position_.column_)))};
      return BinaryenCall(module_.get(), builtin, args, 4, make_nix_value_type());
    }

    BinaryenExpressionRef args[] = {left, right};
    return BinaryenCall(module_.get(), builtin, args, 2, make_nix_value_type());
  }

  /// compile logical AND with short-circuit evaluation
  [[nodiscard]] auto compile_logical_and(const ast::expression_binary_operation& expr)
      -> BinaryenExpressionRef {
    auto left = compile_expression(expr.left_);
    auto right = compile_expression(expr.right_);

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
    auto left = compile_expression(expr.left_);
    auto right = compile_expression(expr.right_);

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
    auto left = compile_expression(expr.left_);
    auto right = compile_expression(expr.right_);

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
    auto arg = compile_expression(expr.left_);
    auto func = compile_expression(expr.right_);
    BinaryenExpressionRef args[] = {func, arg};
    return BinaryenCall(module_.get(), "__apply", args, 2, make_nix_value_type());
  }

  /// compile pipe left: a <| b is a(b)
  [[nodiscard]] auto compile_pipe_left(const ast::expression_binary_operation& expr)
      -> BinaryenExpressionRef {
    auto func = compile_expression(expr.left_);
    auto arg = compile_expression(expr.right_);
    BinaryenExpressionRef args[] = {func, arg};
    return BinaryenCall(module_.get(), "__apply", args, 2, make_nix_value_type());
  }

  /// compile unary operation
  [[nodiscard]] auto compile_variant(const ast::expression_unary_operation& expr)
      -> BinaryenExpressionRef {
    auto operand = compile_expression(expr.operand_);

    const char* builtin = nullptr;
    switch (expr.op_) {
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
    if (expr.parts_.empty()) {
      // empty string
      auto offset = allocate_string("");
      std::int64_t packed =
          (static_cast<std::int64_t>(offset) << 32) | static_cast<std::int64_t>(value_tag::string);
      return BinaryenConst(module_.get(), BinaryenLiteralInt64(packed));
    }

    // optimization: if single literal part, just return that string
    if (expr.parts_.size() == 1 && std::holds_alternative<std::string>(expr.parts_[0])) {
      auto offset = allocate_string(std::get<std::string>(expr.parts_[0]));
      std::int64_t packed =
          (static_cast<std::int64_t>(offset) << 32) | static_cast<std::int64_t>(value_tag::string);
      return BinaryenConst(module_.get(), BinaryenLiteralInt64(packed));
    }

    // general case: compile all parts, coerce to strings, concatenate at runtime
    return compile_interpolation_parts(expr.parts_, value_tag::string);
  }

  [[nodiscard]] auto compile_variant(const ast::expression_path& expr) -> BinaryenExpressionRef {
    // paths are represented as strings with tag=5 (path)
    auto offset = allocate_string(expr.value_);
    std::int64_t packed =
        (static_cast<std::int64_t>(offset) << 32) | static_cast<std::int64_t>(value_tag::path);
    return BinaryenConst(module_.get(), BinaryenLiteralInt64(packed));
  }

  [[nodiscard]] auto compile_variant(const ast::expression_path_interpolated& expr)
      -> BinaryenExpressionRef {
    // path interpolation: similar to string interpolation but returns path type
    if (expr.parts_.empty()) {
      // empty path (unusual but handle it)
      auto offset = allocate_string("");
      std::int64_t packed =
          (static_cast<std::int64_t>(offset) << 32) | static_cast<std::int64_t>(value_tag::path);
      return BinaryenConst(module_.get(), BinaryenLiteralInt64(packed));
    }

    // optimization: if single literal part, just return that path
    if (expr.parts_.size() == 1 && std::holds_alternative<std::string>(expr.parts_[0])) {
      auto offset = allocate_string(std::get<std::string>(expr.parts_[0]));
      std::int64_t packed =
          (static_cast<std::int64_t>(offset) << 32) | static_cast<std::int64_t>(value_tag::path);
      return BinaryenConst(module_.get(), BinaryenLiteralInt64(packed));
    }

    // general case: compile all parts, coerce to strings, concatenate at runtime
    // the result is tagged as a path instead of a string
    return compile_interpolation_parts(expr.parts_, value_tag::path);
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
        expr->data_);
  }

  [[nodiscard]] auto compile_variant(const ast::expression_list& expr) -> BinaryenExpressionRef {
    // compile all elements and store them in memory
    // then call __makeList(offset, count)
    // list elements are lazy - wrap non-trivial expressions in thunks
    auto count = static_cast<std::uint32_t>(expr.elements_.size());

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
    for (std::uint32_t idx = 0; idx < count; ++idx) {
      BinaryenExpressionRef element;
      if (is_trivial_expression(expr.elements_[idx])) {
        // trivial expressions can be evaluated immediately
        element = compile_expression(expr.elements_[idx]);
      } else {
        // non-trivial expressions are wrapped in thunks for lazy evaluation
        element = compile_as_thunk(expr.elements_[idx]);
      }
      auto store = BinaryenStore(module_.get(),
                                 8,                                                     // bytes
                                 offset + idx * 8,                                      // offset
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

    for (auto idx = segments.size(); idx > segments_begin; --idx) {
      const auto& segment = segments[idx - 1];

      if (segment.is_dynamic()) {
        throw compilation_error(
            "dynamic attribute names in multi-segment paths not yet implemented");
      }

      auto sym = std::get<ast::symbol>(segment.value_);
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
      const auto& segments = slice.binding->path_.segments_;
      if (segment_index >= segments.size()) {
        // This binding has no more segments at this level - it's a direct value
        // We handle this case separately
        continue;
      }

      const auto& segment = segments[segment_index];
      if (segment.is_dynamic()) {
        throw compilation_error("dynamic segment in path merging not supported");
      }

      auto sym = std::get<ast::symbol>(segment.value_);
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
        if (attr_binding.path_.segments_.empty()) {
          throw compilation_error("empty attribute path");
        }

        const auto& first_segment = attr_binding.path_.segments_[0];
        if (first_segment.is_dynamic()) {
          throw compilation_error("dynamic first segment in path merging not supported");
        }

        auto first_sym = std::get<ast::symbol>(first_segment.value_);
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
        for (const auto& seg : attr_binding.path_.segments_) {
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
      const auto& segments = slice.binding->path_.segments_;
      if (segment_index >= segments.size()) {
        // This slice ends here - it's a direct value
        if (direct_value != nullptr) {
          throw compilation_error("duplicate attribute definition");
        }
        direct_value = &slice.binding->value_;
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

    // If only direct value, return it wrapped in a thunk for lazy evaluation
    // This is crucial for self-referential attrsets like: let x = { a = 1; b = x; };
    // The value `x` should not be forced until attribute `b` is actually accessed
    if (direct_value != nullptr) {
      // Check if this is an identifier - identifiers must be read WITHOUT forcing
      // because they might hold thunk values that shouldn't be forced during attrset
      // construction. This applies to:
      // - Let-bound variables: let x = { a = 1; b = x; }; (self-reference)
      // - Function parameters: fix (self: { a = 1; b = self; }) (fixpoint pattern)
      // - Captured variables: any closure that captures a thunk
      //
      // We just store the raw value (which might be a thunk). When the attribute
      // is accessed and its value is used, the thunk will be forced then.
      if (std::holds_alternative<ast::expression_identifier>(direct_value->get()->data_)) {
        const auto& ident = std::get<ast::expression_identifier>(direct_value->get()->data_);
        // Read identifier value WITHOUT forcing - preserve thunks as-is
        return compile_identifier_lookup(ident.name_, ident.position_, false);
      } else if (is_trivial_expression(*direct_value)) {
        // Non-identifier trivial expression (literals) - safe to evaluate immediately
        return compile_expression(*direct_value);
      } else {
        // Non-trivial expression - wrap in thunk
        // Use preserve_lazy to avoid forcing captured variables during thunk creation.
        // This is crucial for patterns like: makeExtensible (self: { inner = { lib = self; }; })
        // where `self` is a fixpoint parameter and must not be forced when creating the thunk.
        return compile_as_thunk(*direct_value, capture_policy::preserve_lazy);
      }
    }

    // Group nested slices by the segment at segment_index
    std::unordered_map<ast::symbol, std::vector<binding_path_slice>, symbol_hash> nested_groups;

    for (const auto& slice : nested_slices) {
      const auto& segment = slice.binding->path_.segments_[segment_index];
      if (segment.is_dynamic()) {
        throw compilation_error("dynamic segment in path merging not supported");
      }
      auto sym = std::get<ast::symbol>(segment.value_);
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
    if (expr.is_recursive_) {
      return compile_recursive_attribute_set(expr);
    }

    // Check for dynamic keys - use separate compilation path
    if (has_dynamic_bindings(expr.bindings_)) {
      return compile_attribute_set_with_dynamic_keys(expr);
    }

    // Group bindings by their first segment for multi-segment path merging
    auto grouped = group_bindings_by_first_segment(expr.bindings_);

    // Count total top-level attributes (from grouped bindings and inherit bindings)
    std::uint32_t attr_count = static_cast<std::uint32_t>(grouped.size());

    for (const auto& binding : expr.bindings_) {
      if (std::holds_alternative<ast::binding_inherit>(binding)) {
        const auto& inherit_binding = std::get<ast::binding_inherit>(binding);
        attr_count += static_cast<std::uint32_t>(inherit_binding.attributes_.size());
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
    for (const auto& binding : expr.bindings_) {
      if (std::holds_alternative<ast::binding_inherit>(binding)) {
        const auto& inherit_binding = std::get<ast::binding_inherit>(binding);

        for (const auto& attr_name : inherit_binding.attributes_) {
          // for now only handle static attribute names
          if (attr_name.is_dynamic()) {
            throw compilation_error("dynamic inherit attribute names not yet implemented");
          }

          auto sym = std::get<ast::symbol>(attr_name.value_);
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
          if (inherit_binding.from_expression_.has_value()) {
            // inherit (expr) x; -> select x from expr
            // Wrap in thunk for lazy evaluation - crucial for fixpoint patterns like:
            //   makeExtensible (self: { trivial = {...}; inherit (self.trivial) id; })
            // Without thunking, evaluating self.trivial during attrset construction
            // triggers infinite recursion.
            value = compile_inherit_from_as_thunk(*inherit_binding.from_expression_, key_str,
                                                  attr_name.position_);
          } else {
            // inherit x; -> look up x from outer scope (without forcing)
            value = compile_identifier_lookup(sym, attr_name.position_, false);
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
    for (const auto& binding : expr.bindings_) {
      if (std::holds_alternative<ast::binding_attribute>(binding)) {
        const auto& attr_binding = std::get<ast::binding_attribute>(binding);
        // for now, only single-segment paths are supported with dynamic keys
        if (attr_binding.path_.segments_.size() != 1) {
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

    for (const auto& binding : expr.bindings_) {
      const auto& attr_binding = std::get<ast::binding_attribute>(binding);
      const auto& segment = attr_binding.path_.segments_[0];

      auto pair_offset = offset + pair_index * 16;

      // compile key - either static symbol or dynamic expression
      BinaryenExpressionRef key_value;
      if (segment.is_dynamic()) {
        // dynamic key: compile the expression to get a string value
        const auto& key_expr = std::get<ast::expression>(segment.value_);
        key_value = compile_expression(key_expr);
      } else {
        // static key: create a string constant
        auto sym = std::get<ast::symbol>(segment.value_);
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
      auto value = compile_expression(attr_binding.value_);
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
    for (const auto& binding : expr.bindings_) {
      if (std::holds_alternative<ast::binding_attribute>(binding)) {
        const auto& attr_binding = std::get<ast::binding_attribute>(binding);

        // for now, only single-segment static paths
        if (attr_binding.path_.segments_.empty()) {
          current_scope_ = outer_scope;
          throw compilation_error("empty attribute path in rec");
        }
        if (attr_binding.path_.segments_.size() > 1) {
          current_scope_ = outer_scope;
          throw compilation_error("multi-segment attribute paths in rec not yet implemented");
        }
        const auto& segment = attr_binding.path_.segments_[0];
        if (segment.is_dynamic()) {
          current_scope_ = outer_scope;
          throw compilation_error("dynamic attribute names in rec not yet implemented");
        }

        auto sym = std::get<ast::symbol>(segment.value_);
        auto local_index = current_lambda_context_->next_local_index++;
        current_lambda_context_->local_types.push_back(make_nix_value_type());

        rec_scope.add_local(sym, local_index);
        bindings.push_back({sym, local_index, &attr_binding, nullptr, 0});
      } else {
        const auto& inherit_binding = std::get<ast::binding_inherit>(binding);

        for (std::size_t idx = 0; idx < inherit_binding.attributes_.size(); ++idx) {
          const auto& attr_name = inherit_binding.attributes_[idx];
          if (attr_name.is_dynamic()) {
            current_scope_ = outer_scope;
            throw compilation_error("dynamic inherit names in rec not yet implemented");
          }

          auto sym = std::get<ast::symbol>(attr_name.value_);
          auto local_index = current_lambda_context_->next_local_index++;
          current_lambda_context_->local_types.push_back(make_nix_value_type());

          rec_scope.add_local(sym, local_index);
          bindings.push_back({sym, local_index, nullptr, &inherit_binding, idx});
        }
      }
    }

    std::vector<BinaryenExpressionRef> ops;

    // For rec attrsets with mutual references, we need a different approach:
    // 1. Allocate shared memory for all binding values
    // 2. Create thunks that read from this shared memory (not from captured values)
    // 3. Store thunks in shared memory
    // 4. Thunk bodies read from shared memory at force-time (not capture-time)

    // Allocate shared memory for rec bindings
    auto rec_env_offset = data_offset_;
    auto rec_env_size = static_cast<std::uint32_t>(bindings.size()) * 8; // 8 bytes per nix_value
    data_offset_ += rec_env_size;
    data_offset_ = (data_offset_ + 7) & ~7u; // align

    // Map binding names to their offsets in the shared rec environment
    std::unordered_map<std::uint32_t, std::uint32_t> rec_binding_offsets;
    for (std::uint32_t idx = 0; idx < bindings.size(); ++idx) {
      rec_binding_offsets[bindings[idx].name.index_] = rec_env_offset + idx * 8;
    }

    // Second pass: compile all values and store in shared memory + locals
    for (std::uint32_t idx = 0; idx < bindings.size(); ++idx) {
      const auto& binding = bindings[idx];
      BinaryenExpressionRef value;

      if (binding.attr_binding != nullptr) {
        // regular attribute binding - compile as thunk
        // The thunk will capture the rec_env_offset and read from it at force-time
        value =
            compile_rec_thunk(binding.attr_binding->value_, rec_env_offset, rec_binding_offsets);
      } else {
        // inherit binding
        const auto& inherit = *binding.inherit_binding;
        const auto& attr_name = inherit.attributes_[binding.inherit_attr_index];
        auto sym = std::get<ast::symbol>(attr_name.value_);

        if (inherit.from_expression_.has_value()) {
          // inherit (expr) x; - select from expression
          auto from_value = compile_expression(*inherit.from_expression_);
          auto key_str = symbols_.lookup(sym);
          value = compile_select(from_value, key_str, attr_name.position_);
        } else {
          // inherit x; - look up from outer scope
          current_scope_ = outer_scope;
          value = compile_identifier_lookup(sym, attr_name.position_);
          current_scope_ = &rec_scope;
        }
      }

      // Store in shared memory location
      auto store_to_rec_env = BinaryenStore(module_.get(), 8, rec_env_offset + idx * 8, 0,
                                            BinaryenConst(module_.get(), BinaryenLiteralInt32(0)),
                                            value, BinaryenTypeInt64(), "memory");
      ops.push_back(store_to_rec_env);

      // Also store in local (for attrset construction)
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

    for (std::uint32_t idx = 0; idx < attr_count; ++idx) {
      const auto& binding = bindings[idx];
      auto key_str = symbols_.lookup(binding.name);
      auto key_offset = allocate_string(key_str);

      auto pair_offset = offset + idx * 12;

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
    if (expr.path_.segments_.empty()) {
      throw compilation_error("empty attribute path");
    }

    // If there's no default value, use the simple selection path (can throw on missing attr)
    if (!expr.default_value_.has_value()) {
      auto result = compile_expression(expr.subject_);
      for (const auto& segment : expr.path_.segments_) {
        if (segment.is_dynamic()) {
          const auto& key_expr = std::get<ast::expression>(segment.value_);
          auto key_value = compile_expression(key_expr);
          result = compile_select_dynamic(result, key_value, segment.position_);
        } else {
          auto sym = std::get<ast::symbol>(segment.value_);
          auto key_str = symbols_.lookup(sym);
          result = compile_select(result, key_str, segment.position_);
        }
      }
      return result;
    }

    // With default value: check hasAttr before each select.
    // If any attribute is missing, return the default value.
    // Generate nested conditionals that short-circuit on missing attributes.
    auto default_val = compile_expression(*expr.default_value_);
    auto subject = compile_expression(expr.subject_);

    // Build the selection chain with hasAttr checks
    // For a.b.c or default, we generate:
    //   if hasAttr(a, "b") then
    //     let tmp = a.b in
    //     if hasAttr(tmp, "c") then tmp.c else default
    //   else default
    return compile_select_with_default(subject, expr.path_.segments_, 0, default_val);
  }

  /// Helper to compile a select with default, handling each path segment recursively
  [[nodiscard]] auto compile_select_with_default(BinaryenExpressionRef current,
                                                 const std::vector<ast::attribute_name>& segments,
                                                 std::size_t idx, BinaryenExpressionRef default_val)
      -> BinaryenExpressionRef {
    if (idx >= segments.size()) {
      return current; // All segments processed, return the final value
    }

    const auto& segment = segments[idx];
    bool is_last = (idx == segments.size() - 1);

    // Build hasAttr check for this segment
    BinaryenExpressionRef has_attr_result;
    if (segment.is_dynamic()) {
      const auto& key_expr = std::get<ast::expression>(segment.value_);
      auto key_value = compile_expression(key_expr);
      BinaryenExpressionRef args[] = {current, key_value};
      has_attr_result =
          BinaryenCall(module_.get(), "__hasAttrDynamic", args, 2, make_nix_value_type());
    } else {
      auto sym = std::get<ast::symbol>(segment.value_);
      auto key_str = symbols_.lookup(sym);
      auto key_offset = allocate_string(key_str);
      BinaryenExpressionRef args[] = {
          current, BinaryenConst(module_.get(),
                                 BinaryenLiteralInt32(static_cast<std::int32_t>(key_offset)))};
      has_attr_result = BinaryenCall(module_.get(), "__hasAttr", args, 2, make_nix_value_type());
    }

    auto has_attr_is_true = BinaryenBinary(
        module_.get(), BinaryenEqInt64(),
        BinaryenConst(module_.get(), BinaryenLiteralInt64(packed::boolean_true)), has_attr_result);

    // Build select for this segment (only evaluated if hasAttr is true)
    BinaryenExpressionRef select_result;
    if (segment.is_dynamic()) {
      const auto& key_expr = std::get<ast::expression>(segment.value_);
      auto key_value = compile_expression(key_expr);
      select_result = compile_select_dynamic(current, key_value, segment.position_);
    } else {
      auto sym = std::get<ast::symbol>(segment.value_);
      auto key_str = symbols_.lookup(sym);
      select_result = compile_select(current, key_str, segment.position_);
    }

    BinaryenExpressionRef then_branch;
    if (is_last) {
      // Last segment: just return the selected value
      then_branch = select_result;
    } else {
      // More segments to go: recursively process the rest
      then_branch = compile_select_with_default(select_result, segments, idx + 1, default_val);
    }

    // if hasAttr then continue else default
    return BinaryenIf(module_.get(), has_attr_is_true, then_branch, default_val);
  }

  [[nodiscard]] auto compile_variant(const ast::expression_has_attribute& expr)
      -> BinaryenExpressionRef {
    auto subject = compile_expression(expr.subject_);

    if (expr.path_.segments_.empty()) {
      // empty path always true
      return BinaryenConst(module_.get(), BinaryenLiteralInt64(packed::boolean_true));
    }

    // for multi-segment paths, chain hasAttr and select operations
    // e.g., x ? a.b.c means: hasAttr(x, "a") && hasAttr(x.a, "b") && hasAttr(x.a.b, "c")
    auto result = subject;
    for (std::size_t idx = 0; idx < expr.path_.segments_.size(); ++idx) {
      const auto& segment = expr.path_.segments_[idx];
      bool is_last = (idx == expr.path_.segments_.size() - 1);

      BinaryenExpressionRef has_attr_result;
      if (segment.is_dynamic()) {
        // dynamic key: compile the expression and use __hasAttrDynamic
        const auto& key_expr = std::get<ast::expression>(segment.value_);
        auto key_value = compile_expression(key_expr);

        BinaryenExpressionRef args[] = {result, key_value};
        has_attr_result =
            BinaryenCall(module_.get(), "__hasAttrDynamic", args, 2, make_nix_value_type());
      } else {
        // static key: use __hasAttr with string offset
        auto sym = std::get<ast::symbol>(segment.value_);
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
        const auto& key_expr = std::get<ast::expression>(segment.value_);
        auto key_value = compile_expression(key_expr);
        select_result = compile_select_dynamic(result, key_value, segment.position_);
      } else {
        auto sym = std::get<ast::symbol>(segment.value_);
        auto key_str = symbols_.lookup(sym);
        select_result = compile_select(result, key_str, segment.position_);
      }

      // wrap in a conditional: if hasAttr is true, continue with select_result; else return false
      // we need to continue with the rest of the path, so we can't directly return here
      // Instead, use the selected value for the next iteration, but guard with the check
      // Note: we check if has_attr_result equals boolean_true
      auto has_attr_is_true =
          BinaryenBinary(module_.get(), BinaryenEqInt64(),
                         BinaryenConst(module_.get(), BinaryenLiteralInt64(packed::boolean_true)),
                         has_attr_result);

      // simplify for now: assume all hasAttr checks pass (we'll evaluate fully)
      // the proper solution would require temp locals, which is complex
      // TODO: use has_attr_is_true to short-circuit when hasAttr returns false
      (void)has_attr_is_true;
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
    const auto& pattern = *expr.argument_pattern_;
    if (std::holds_alternative<ast::pattern_simple>(pattern)) {
      const auto& simple = std::get<ast::pattern_simple>(pattern);
      bound_names.push_back(simple.argument_name_);
    } else {
      const auto& attrset_pattern = std::get<ast::pattern_attrset>(pattern);
      if (attrset_pattern.argument_name_.has_value()) {
        bound_names.push_back(*attrset_pattern.argument_name_);
      }
      for (const auto& formal : attrset_pattern.formals_) {
        bound_names.push_back(formal.name_);
      }
    }

    // analyze free variables in the lambda body AND default values
    // Default values are evaluated in the outer scope, so they may reference
    // variables that are not bound by the lambda itself.
    auto free_vars = free_variable_analyzer::analyze(expr.body_, bound_names);

    // Also analyze default values (they reference outer scope, not lambda bindings)
    if (std::holds_alternative<ast::pattern_attrset>(pattern)) {
      const auto& attrset_pattern = std::get<ast::pattern_attrset>(pattern);
      for (const auto& formal : attrset_pattern.formals_) {
        if (formal.default_value_.has_value()) {
          // Default values are NOT in scope of lambda bindings, so we analyze
          // with an empty bound_names to find all their free variables
          auto default_free = free_variable_analyzer::analyze(*formal.default_value_, {});
          for (auto sym : default_free) {
            // Only add if not already in lambda bindings (they shadow outer scope)
            if (std::find(bound_names.begin(), bound_names.end(), sym) == bound_names.end()) {
              free_vars.push_back(sym);
            }
          }
        }
      }
    }

    // filter free variables: only keep those that are actually in scope
    // (others are builtins or globals that will be looked up at runtime)
    // Also exclude let-bound and rec-bound variables - they're accessed via shared memory
    std::vector<ast::symbol> captures;
    for (auto sym : free_vars) {
      // Skip variables in let scope (accessed via shared memory)
      if (current_let_binding_offsets_.count(sym.index_) > 0) {
        continue;
      }
      // Skip variables in rec scope (accessed via shared memory)
      if (current_rec_binding_offsets_.count(sym.index_) > 0) {
        continue;
      }
      if (current_scope_ && current_scope_->lookup(sym).has_value()) {
        captures.push_back(sym);
      }
    }

    // save current lambda context
    auto outer_lambda_context = std::move(current_lambda_context_);
    current_lambda_context_ = lambda_context{};
    current_lambda_context_->captures = captures;

    // build capture index map
    for (std::uint32_t idx = 0; idx < captures.size(); ++idx) {
      current_lambda_context_->capture_indices[captures[idx]] = idx;
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
    for (std::uint32_t idx = 0; idx < captures.size(); ++idx) {
      lambda_scope.add_captured(captures[idx], idx);
    }

    // add the argument binding to the scope
    std::uint32_t arg_local_index = 1;

    // handle the pattern binding
    BinaryenExpressionRef pattern_setup = nullptr;

    if (std::holds_alternative<ast::pattern_simple>(pattern)) {
      // simple pattern: x: body
      const auto& simple = std::get<ast::pattern_simple>(pattern);
      lambda_scope.add_local(simple.argument_name_, arg_local_index);
    } else {
      // attrset pattern: { a, b ? default, ... }@name: body
      const auto& attrset_pattern = std::get<ast::pattern_attrset>(pattern);

      // if there's an @name binding, bind the whole argument
      if (attrset_pattern.argument_name_.has_value()) {
        lambda_scope.add_local(*attrset_pattern.argument_name_, arg_local_index);
      }

      // for each formal parameter, we need to extract it from the argument attrset
      std::vector<BinaryenExpressionRef> setup_ops;

      for (const auto& formal : attrset_pattern.formals_) {
        // allocate a local for this formal parameter
        auto formal_local = current_lambda_context_->next_local_index++;
        current_lambda_context_->local_types.push_back(make_nix_value_type());
        lambda_scope.add_local(formal.name_, formal_local);

        // generate code to extract the attribute from the argument
        auto key_str = symbols_.lookup(formal.name_);
        auto key_offset = allocate_string(key_str);

        BinaryenExpressionRef value_expr;
        if (formal.default_value_.has_value()) {
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
          auto selected_value = compile_select(arg_value_select, key_str, formal.position_);

          // else branch: use default value
          auto default_val = compile_expression(*formal.default_value_);

          value_expr = BinaryenIf(module_.get(), condition, selected_value, default_val);
        } else {
          // no default: just select (will error if not found)
          auto arg_value = BinaryenLocalGet(module_.get(), arg_local_index, make_nix_value_type());
          value_expr = compile_select(arg_value, key_str, formal.position_);
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
    auto body_expr = compile_expression(expr.body_);

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
    for (std::size_t idx = 2; idx < current_lambda_context_->local_types.size(); ++idx) {
      local_types.push_back(current_lambda_context_->local_types[idx]);
    }

    BinaryenAddFunction(module_.get(), func_name.c_str(), params, make_nix_value_type(),
                        local_types.empty() ? nullptr : local_types.data(),
                        static_cast<BinaryenIndex>(local_types.size()), full_body);

    // restore scope and context
    current_scope_ = outer_scope;
    auto captured_vars = std::move(current_lambda_context_->captures);
    current_lambda_context_ = std::move(outer_lambda_context);

    // Create closure value via __makeClosure host call.
    // This allows the runtime to encode the module_id for cross-module lambda support.
    //
    // We first build an "environment" in the data segment containing:
    //   [capture_count: i32, captures...: nix_value[]]
    // Then call __makeClosure(func_index, env_offset, env_size) which:
    //   1. Encodes the module_id into func_index
    //   2. Allocates the closure on the heap
    //   3. Returns the packed closure value
    auto capture_count = static_cast<std::uint32_t>(captured_vars.size());
    auto env_size = 4 + capture_count * 8; // capture_count (i32) + captures

    // Allocate environment in data segment
    auto env_offset = data_offset_;
    data_offset_ += env_size;
    data_offset_ = (data_offset_ + 7) & ~7u; // align to 8

    if (captured_vars.empty()) {
      // Zero-capture lambda: environment is just [capture_count=0]
      std::vector<char> env_data(4, 0); // capture_count = 0
      BinaryenAddDataSegment(module_.get(), nullptr, "memory", false,
                             BinaryenConst(module_.get(), BinaryenLiteralInt32(env_offset)),
                             env_data.data(), 4);

      // Call __makeClosure(func_index, env_offset, env_size)
      BinaryenExpressionRef args[] = {
          BinaryenConst(module_.get(), BinaryenLiteralInt32(static_cast<std::int32_t>(func_index))),
          BinaryenConst(module_.get(), BinaryenLiteralInt32(static_cast<std::int32_t>(env_offset))),
          BinaryenConst(module_.get(), BinaryenLiteralInt32(static_cast<std::int32_t>(env_size))),
      };
      return BinaryenCall(module_.get(), "__makeClosure", args, 3, make_nix_value_type());
    }

    // Lambda with captures: build environment at runtime
    std::vector<BinaryenExpressionRef> env_setup;

    // Store capture_count at env_offset
    env_setup.push_back(BinaryenStore(
        module_.get(), 4, env_offset, 0, BinaryenConst(module_.get(), BinaryenLiteralInt32(0)),
        BinaryenConst(module_.get(),
                      BinaryenLiteralInt32(static_cast<std::int32_t>(capture_count))),
        BinaryenTypeInt32(), "memory"));

    // Store each captured value at env_offset + 4 + idx*8
    // IMPORTANT: Don't force captured values! They remain lazy.
    // This is critical for builtins like tryEval that need to catch errors.
    for (std::uint32_t idx = 0; idx < capture_count; ++idx) {
      auto sym = captured_vars[idx];
      auto var_value = compile_identifier_lookup(sym, {0, 0, 0}, false); // Don't force
      env_setup.push_back(BinaryenStore(module_.get(), 8, env_offset + 4 + idx * 8, 0,
                                        BinaryenConst(module_.get(), BinaryenLiteralInt32(0)),
                                        var_value, BinaryenTypeInt64(), "memory"));
    }

    // Call __makeClosure(func_index, env_offset, env_size)
    BinaryenExpressionRef args[] = {
        BinaryenConst(module_.get(), BinaryenLiteralInt32(static_cast<std::int32_t>(func_index))),
        BinaryenConst(module_.get(), BinaryenLiteralInt32(static_cast<std::int32_t>(env_offset))),
        BinaryenConst(module_.get(), BinaryenLiteralInt32(static_cast<std::int32_t>(env_size))),
    };
    env_setup.push_back(
        BinaryenCall(module_.get(), "__makeClosure", args, 3, make_nix_value_type()));

    return BinaryenBlock(module_.get(), nullptr, env_setup.data(),
                         static_cast<BinaryenIndex>(env_setup.size()), make_nix_value_type());
  }

  [[nodiscard]] auto compile_variant(const ast::expression_application& expr)
      -> BinaryenExpressionRef {
    // compile function
    auto func = compile_expression(expr.function_);

    // apply each argument in sequence (curried application)
    // Arguments are wrapped in thunks for lazy evaluation (Nix is lazy)
    // This is crucial for builtins like tryEval that need to catch errors
    auto result = func;
    for (const auto& arg : expr.arguments_) {
      BinaryenExpressionRef compiled_arg;

      // Check if arg is an identifier that's in let scope - pass without forcing
      bool is_let_bound_identifier = false;
      if (std::holds_alternative<ast::expression_identifier>(arg->data_)) {
        const auto& ident = std::get<ast::expression_identifier>(arg->data_);
        if (current_let_binding_offsets_.count(ident.name_.index_) > 0) {
          is_let_bound_identifier = true;
        }
      }

      if (is_let_bound_identifier) {
        // Let-bound identifier - read from shared memory WITHOUT forcing
        // This is crucial for patterns like: let result = overlay result base; in result
        // where `result` is passed to a function before it's fully evaluated
        const auto& ident = std::get<ast::expression_identifier>(arg->data_);
        auto let_it = current_let_binding_offsets_.find(ident.name_.index_);
        auto mem_offset = let_it->second;
        compiled_arg =
            BinaryenLoad(module_.get(), 8, 0, mem_offset, 0, BinaryenTypeInt64(),
                         BinaryenConst(module_.get(), BinaryenLiteralInt32(0)), "memory");
      } else if (std::holds_alternative<ast::expression_identifier>(arg->data_)) {
        // Identifiers: pass WITHOUT forcing when used as function arguments.
        // The called function will force if/when needed. This is critical for
        // builtins like tryEval that need to catch errors during forcing.
        const auto& ident = std::get<ast::expression_identifier>(arg->data_);
        compiled_arg = compile_identifier_lookup(ident.name_, ident.position_, false);
      } else if (is_trivial_expression(arg)) {
        // trivial expressions (literals) can be evaluated immediately
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
    // Let bindings in Nix are mutually recursive (like OCaml's "let rec")
    std::uint32_t scope_depth = current_scope_ ? current_scope_->depth() : 0;
    lexical_scope let_scope(current_scope_, scope_depth);
    auto* outer_scope = current_scope_;
    current_scope_ = &let_scope;

    // Group bindings by first segment for multi-segment path merging
    auto grouped = group_bindings_by_first_segment(expr.bindings_);

    // Check for dynamic first segments (not supported in let)
    for (const auto& binding : expr.bindings_) {
      if (std::holds_alternative<ast::binding_attribute>(binding)) {
        const auto& attr_binding = std::get<ast::binding_attribute>(binding);
        if (attr_binding.path_.segments_.empty()) {
          throw compilation_error("empty attribute path in let binding");
        }
        const auto& first_segment = attr_binding.path_.segments_[0];
        if (first_segment.is_dynamic()) {
          throw compilation_error("dynamic let binding names not yet implemented");
        }
      }
    }

    // ========================================================================
    // PASS 1: Allocate locals and add to scope for ALL bindings first
    // This enables mutual recursion - all bindings are in scope when compiling values
    // ========================================================================

    // Track grouped bindings: symbol -> (local_index, slices)
    struct grouped_binding_info {
      ast::symbol sym;
      std::uint32_t local_index;
      std::vector<binding_path_slice> slices;
    };
    std::vector<grouped_binding_info> grouped_bindings;

    for (const auto& [first_sym, slices] : grouped) {
      std::uint32_t local_index;
      if (current_lambda_context_.has_value()) {
        local_index = current_lambda_context_->next_local_index++;
        current_lambda_context_->local_types.push_back(make_nix_value_type());
      } else {
        throw compilation_error("top-level let expressions not supported");
      }
      let_scope.add_local(first_sym, local_index);
      grouped_bindings.push_back(grouped_binding_info{first_sym, local_index, slices});
    }

    // Track inherit bindings: symbol -> (local_index, inherit_binding ptr, attr_index)
    struct inherit_binding_info {
      ast::symbol sym;
      std::uint32_t local_index;
      const ast::binding_inherit* inherit_binding;
      std::size_t attr_index;
    };
    std::vector<inherit_binding_info> inherit_bindings;

    for (const auto& binding : expr.bindings_) {
      if (std::holds_alternative<ast::binding_inherit>(binding)) {
        const auto& inherit_binding = std::get<ast::binding_inherit>(binding);
        for (std::size_t idx = 0; idx < inherit_binding.attributes_.size(); ++idx) {
          const auto& attr_name = inherit_binding.attributes_[idx];
          if (attr_name.is_dynamic()) {
            throw compilation_error("dynamic inherit attribute names not yet implemented");
          }
          auto sym = std::get<ast::symbol>(attr_name.value_);

          std::uint32_t local_index;
          if (current_lambda_context_.has_value()) {
            local_index = current_lambda_context_->next_local_index++;
            current_lambda_context_->local_types.push_back(make_nix_value_type());
          } else {
            throw compilation_error("top-level let expressions not supported");
          }
          let_scope.add_local(sym, local_index);
          inherit_bindings.push_back({sym, local_index, &inherit_binding, idx});
        }
      }
    }

    // ========================================================================
    // PASS 2: Allocate shared memory and set up let binding offsets
    // This enables recursive let bindings (closures can reference other let bindings)
    // ========================================================================

    auto total_bindings = grouped_bindings.size() + inherit_bindings.size();
    auto let_env_offset = data_offset_;
    auto let_env_size = static_cast<std::uint32_t>(total_bindings) * 8; // 8 bytes per nix_value
    data_offset_ += let_env_size;
    data_offset_ = (data_offset_ + 7) & ~7u; // align

    // Build the let binding offsets map
    std::unordered_map<std::uint32_t, std::uint32_t> let_binding_offsets;
    std::uint32_t offset_idx = 0;
    for (const auto& info : grouped_bindings) {
      let_binding_offsets[info.sym.index_] = let_env_offset + offset_idx * 8;
      offset_idx++;
    }
    for (const auto& info : inherit_bindings) {
      let_binding_offsets[info.sym.index_] = let_env_offset + offset_idx * 8;
      offset_idx++;
    }

    // Save outer let binding offsets and set current
    auto outer_let_offsets = std::move(current_let_binding_offsets_);
    current_let_binding_offsets_ = let_binding_offsets;

    // ========================================================================
    // PASS 3: Compile all values and store in shared memory + locals
    // ========================================================================

    std::vector<BinaryenExpressionRef> binding_ops;

    // Compile grouped attribute bindings as thunks for lazy evaluation
    // All let bindings must be thunks to support forward/mutual references
    offset_idx = 0;
    for (const auto& info : grouped_bindings) {
      BinaryenExpressionRef value;

      // Check if this is a single simple binding (one path segment, direct value)
      if (info.slices.size() == 1 && info.slices[0].binding->path_.segments_.size() == 1) {
        // Single simple binding - wrap the value expression in a thunk
        // This is critical for forward references like: let x = y; y = 1; in x
        // Use preserve_lazy because let bindings are mutually recursive
        // and we don't want to force captured values during thunk creation.
        // Example: let foo = { lib = self; }; in ... where self is a fixpoint.
        value = compile_as_thunk(info.slices[0].binding->value_, capture_policy::preserve_lazy);
      } else {
        // Multi-segment path or merged binding - use normal merging logic
        // This already wraps non-trivial sub-expressions in thunks
        value = compile_merged_attrset_value(info.slices, 1);
      }

      // Store in shared memory (for closure captures to read from)
      auto store_to_mem = BinaryenStore(module_.get(), 8, let_env_offset + offset_idx * 8, 0,
                                        BinaryenConst(module_.get(), BinaryenLiteralInt32(0)),
                                        value, BinaryenTypeInt64(), "memory");
      binding_ops.push_back(store_to_mem);

      // Also store in local (for direct access in the let body)
      auto store_local = BinaryenLocalSet(module_.get(), info.local_index, value);
      binding_ops.push_back(store_local);
      offset_idx++;
    }

    // Compile inherit bindings
    // Each inherit binding is wrapped in a thunk for lazy evaluation
    // This is critical for circular dependencies like:
    //   let inherit (lib.trivial) isFunction; lib = { trivial = ...; }; in ...
    // The inherit must not be evaluated until isFunction is actually used.
    for (const auto& info : inherit_bindings) {
      BinaryenExpressionRef value;

      if (info.inherit_binding->from_expression_.has_value()) {
        // inherit (expr) x; -> create a thunk that does: (expr).x
        // We can't just compile (expr) here because that would force it eagerly.
        // Instead, create a select expression and wrap it in a thunk.
        auto key_str = symbols_.lookup(info.sym);
        auto key_offset = allocate_string(key_str);

        // Generate a unique function name for this thunk
        auto func_index = thunk_counter_++;
        auto func_name = "__thunk_" + std::to_string(func_index);
        thunk_function_names_.push_back(func_name);

        // The thunk body will compile the from_expression and select
        // Save current context
        auto outer_lambda_context = std::move(current_lambda_context_);
        current_lambda_context_ = lambda_context{};

        // Thunk needs to capture variables from from_expression
        std::vector<ast::symbol> bound_names;
        auto free_vars =
            free_variable_analyzer::analyze(*info.inherit_binding->from_expression_, bound_names);

        // Filter captures (excluding let-bound vars, accessed via shared memory)
        std::vector<ast::symbol> captures;
        for (auto sym : free_vars) {
          if (current_let_binding_offsets_.count(sym.index_) > 0)
            continue;
          if (current_rec_binding_offsets_.count(sym.index_) > 0)
            continue;
          if (outer_scope && outer_scope->lookup(sym).has_value())
            captures.push_back(sym);
        }

        current_lambda_context_->captures = captures;
        for (std::uint32_t idx = 0; idx < captures.size(); ++idx) {
          current_lambda_context_->capture_indices[captures[idx]] = idx;
        }

        // Create thunk scope
        std::uint32_t new_depth = outer_scope ? outer_scope->depth() + 1 : 1;
        lexical_scope thunk_scope(nullptr, new_depth);
        current_scope_ = &thunk_scope;

        // Thunk takes only env_ptr
        current_lambda_context_->local_types.push_back(BinaryenTypeInt32());
        current_lambda_context_->next_local_index = 1;

        // Add captures to thunk scope
        for (std::uint32_t idx = 0; idx < captures.size(); ++idx) {
          thunk_scope.add_captured(captures[idx], idx);
        }

        // Compile: from_expr.attr_name
        auto from_expr = compile_expression(*info.inherit_binding->from_expression_);
        auto thunk_body = compile_select(
            from_expr, key_str, info.inherit_binding->attributes_[info.attr_index].position_);

        // Create the thunk function
        BinaryenType param_types[] = {BinaryenTypeInt32()};
        auto params = BinaryenTypeCreate(param_types, 1);

        std::vector<BinaryenType> local_types;
        for (std::size_t idx = 1; idx < current_lambda_context_->local_types.size(); ++idx) {
          local_types.push_back(current_lambda_context_->local_types[idx]);
        }

        BinaryenAddFunction(module_.get(), func_name.c_str(), params, make_nix_value_type(),
                            local_types.empty() ? nullptr : local_types.data(),
                            static_cast<BinaryenIndex>(local_types.size()), thunk_body);

        // Restore context
        current_scope_ = &let_scope;
        auto captured_vars = std::move(current_lambda_context_->captures);
        current_lambda_context_ = std::move(outer_lambda_context);

        // Create the thunk value
        if (captured_vars.empty()) {
          BinaryenExpressionRef make_thunk_args[] = {
              BinaryenConst(module_.get(),
                            BinaryenLiteralInt32(static_cast<std::int32_t>(func_index))),
              BinaryenConst(module_.get(), BinaryenLiteralInt32(0)),
              BinaryenConst(module_.get(), BinaryenLiteralInt32(0))};
          value =
              BinaryenCall(module_.get(), "__makeThunk", make_thunk_args, 3, make_nix_value_type());
        } else {
          // Allocate and populate environment
          auto capture_count = static_cast<std::uint32_t>(captured_vars.size());
          auto env_size = 4 + capture_count * 8;
          auto env_offset = data_offset_;
          data_offset_ += env_size;
          data_offset_ = (data_offset_ + 7) & ~7u;

          std::vector<BinaryenExpressionRef> store_ops;

          // Store capture count
          auto store_count = BinaryenStore(
              module_.get(), 4, env_offset, 0,
              BinaryenConst(module_.get(), BinaryenLiteralInt32(0)),
              BinaryenConst(module_.get(),
                            BinaryenLiteralInt32(static_cast<std::int32_t>(capture_count))),
              BinaryenTypeInt32(), "memory");
          store_ops.push_back(store_count);

          // Store captured values using preserve_lazy semantics (don't force).
          // This is critical for fixpoint patterns like:
          //   fix (self: { trivial = let inherit (self.trivial) x; in {...}; })
          // Here `self` is captured but must not be forced during thunk creation.
          auto* saved_scope = current_scope_;
          current_scope_ = outer_scope;
          for (std::uint32_t idx = 0; idx < capture_count; ++idx) {
            auto cap_value =
                compile_identifier_lookup(captured_vars[idx], {0, 0, 0},
                                          should_force_captures(capture_policy::preserve_lazy));
            auto store_cap = BinaryenStore(module_.get(), 8, env_offset + 4 + idx * 8, 0,
                                           BinaryenConst(module_.get(), BinaryenLiteralInt32(0)),
                                           cap_value, BinaryenTypeInt64(), "memory");
            store_ops.push_back(store_cap);
          }
          current_scope_ = saved_scope;

          // Create thunk with environment
          BinaryenExpressionRef make_thunk_args[] = {
              BinaryenConst(module_.get(),
                            BinaryenLiteralInt32(static_cast<std::int32_t>(func_index))),
              BinaryenConst(module_.get(),
                            BinaryenLiteralInt32(static_cast<std::int32_t>(env_offset))),
              BinaryenConst(module_.get(),
                            BinaryenLiteralInt32(static_cast<std::int32_t>(env_size)))};
          auto make_thunk =
              BinaryenCall(module_.get(), "__makeThunk", make_thunk_args, 3, make_nix_value_type());
          store_ops.push_back(make_thunk);

          value =
              BinaryenBlock(module_.get(), nullptr, store_ops.data(),
                            static_cast<BinaryenIndex>(store_ops.size()), make_nix_value_type());
        }
      } else {
        // inherit x; -> look up x from outer scope (still wrap in thunk for consistency)
        auto* saved_scope = current_scope_;
        current_scope_ = outer_scope;
        // For simple inherit without from, just read the identifier without forcing
        // It may already be a thunk if the inherited var is a let binding
        value = compile_identifier_lookup(
            info.sym, info.inherit_binding->attributes_[info.attr_index].position_, false);
        current_scope_ = saved_scope;
      }

      // Store in shared memory
      auto store_to_mem = BinaryenStore(module_.get(), 8, let_env_offset + offset_idx * 8, 0,
                                        BinaryenConst(module_.get(), BinaryenLiteralInt32(0)),
                                        value, BinaryenTypeInt64(), "memory");
      binding_ops.push_back(store_to_mem);

      // Also store in local
      auto store_local = BinaryenLocalSet(module_.get(), info.local_index, value);
      binding_ops.push_back(store_local);
      offset_idx++;
    }

    // Restore outer let binding offsets
    current_let_binding_offsets_ = std::move(outer_let_offsets);

    // compile the body
    auto body_expr = compile_expression(expr.body_);

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
    auto namespace_expr = compile_expression(expr.namespace_expression_);

    // store namespace in the local
    auto store_namespace = BinaryenLocalSet(module_.get(), namespace_local, namespace_expr);

    // push with scope
    with_scopes_.push_back({namespace_local});

    // compile the body
    auto body_expr = compile_expression(expr.body_);

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
                      BinaryenLiteralInt32(static_cast<std::int32_t>(position.line_))),
        BinaryenConst(module_.get(),
                      BinaryenLiteralInt32(static_cast<std::int32_t>(position.column_)))};
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
                      BinaryenLiteralInt32(static_cast<std::int32_t>(position.line_))),
        BinaryenConst(module_.get(),
                      BinaryenLiteralInt32(static_cast<std::int32_t>(position.column_)))};
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
                      BinaryenLiteralInt32(static_cast<std::int32_t>(position.line_))),
        BinaryenConst(module_.get(),
                      BinaryenLiteralInt32(static_cast<std::int32_t>(position.column_)))};
    return BinaryenCall(module_.get(), "__selectDynamic", args, 4, make_nix_value_type());
  }

  [[nodiscard]] auto compile_variant(const ast::expression_if& expr) -> BinaryenExpressionRef {
    auto condition = compile_expression(expr.condition_);
    auto then_branch = compile_expression(expr.then_branch_);
    auto else_branch = compile_expression(expr.else_branch_);

    // Call __expectBool to force and type-check the condition
    BinaryenExpressionRef expect_args[] = {
        condition,
        BinaryenConst(module_.get(),
                      BinaryenLiteralInt32(static_cast<std::int32_t>(expr.position_.line_))),
        BinaryenConst(module_.get(),
                      BinaryenLiteralInt32(static_cast<std::int32_t>(expr.position_.column_)))};
    auto validated_condition =
        BinaryenCall(module_.get(), "__expectBool", expect_args, 3, make_nix_value_type());

    // condition must be a boolean - check if it equals true
    auto cond_is_true =
        BinaryenBinary(module_.get(), BinaryenEqInt64(),
                       BinaryenConst(module_.get(), BinaryenLiteralInt64(packed::boolean_true)),
                       validated_condition);

    return BinaryenIf(module_.get(), cond_is_true, then_branch, else_branch);
  }

  [[nodiscard]] auto compile_variant(const ast::expression_assert& expr) -> BinaryenExpressionRef {
    auto condition = compile_expression(expr.condition_);
    auto body = compile_expression(expr.body_);

    // Call __expectBool to force and type-check the condition
    BinaryenExpressionRef expect_args[] = {
        condition,
        BinaryenConst(module_.get(),
                      BinaryenLiteralInt32(static_cast<std::int32_t>(expr.position_.line_))),
        BinaryenConst(module_.get(),
                      BinaryenLiteralInt32(static_cast<std::int32_t>(expr.position_.column_)))};
    auto validated_condition =
        BinaryenCall(module_.get(), "__expectBool", expect_args, 3, make_nix_value_type());

    // check if condition is true
    auto cond_is_true =
        BinaryenBinary(module_.get(), BinaryenEqInt64(),
                       BinaryenConst(module_.get(), BinaryenLiteralInt64(packed::boolean_true)),
                       validated_condition);

    // if false, throw with position; otherwise return body
    auto throw_expr = compile_throw("assertion failed", expr.position_);

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

    // Check data segment limit (256KB - increased for large nixpkgs files)
    constexpr std::uint32_t data_segment_limit = 0x40000;
    if (data_offset_ + total_size > data_segment_limit) {
      throw compilation_error("data segment overflow: expression too large (limit: 256KB)");
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

} // namespace straylight::nix::compiler::compile
