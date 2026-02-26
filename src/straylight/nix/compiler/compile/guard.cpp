/// @file straylight/nix/compiler/compile/guard.cpp
/// Implementation of RAII guards for compiler state.

#include "straylight/nix/compiler/compile/guard.h"

#include "straylight/nix/compiler/compile/compiler.h"

namespace straylight::nix::compiler::compile {

// =============================================================================
// scope_guard
// =============================================================================

scope_guard::scope_guard(compiler& c, lexical_scope& new_scope)
    : compiler_(c), saved_scope_(c.current_scope_) {
  compiler_.current_scope_ = &new_scope;
}

scope_guard::~scope_guard() {
  compiler_.current_scope_ = saved_scope_;
}

// =============================================================================
// lambda_context_guard
// =============================================================================

lambda_context_guard::lambda_context_guard(compiler& c)
    : compiler_(c), saved_context_(nullptr), captured_symbols_(), captures_taken_(false) {
  // Save via heap allocation to avoid lambda_context definition in header
  auto* saved = new std::optional<compiler::lambda_context>(std::move(c.current_lambda_context_));
  saved_context_ = saved;
  c.current_lambda_context_ = compiler::lambda_context{};
}

lambda_context_guard::~lambda_context_guard() {
  // If captures weren't taken, extract them now (they'll be discarded but that's OK)
  if (!captures_taken_ && compiler_.current_lambda_context_.has_value()) {
    captured_symbols_ = std::move(compiler_.current_lambda_context_->captures);
  }
  auto* saved = static_cast<std::optional<compiler::lambda_context>*>(saved_context_);
  compiler_.current_lambda_context_ = std::move(*saved);
  delete saved;
}

auto lambda_context_guard::take_captures() -> std::vector<ast::symbol> {
  if (captures_taken_) {
    return {}; // Already taken
  }
  captures_taken_ = true;
  if (compiler_.current_lambda_context_.has_value()) {
    captured_symbols_ = std::move(compiler_.current_lambda_context_->captures);
  }
  return captured_symbols_;
}

// =============================================================================
// rec_bindings_guard
// =============================================================================

rec_bindings_guard::rec_bindings_guard(
    compiler& c, const std::unordered_map<std::uint32_t, std::uint32_t>& new_offsets)
    : compiler_(c), saved_offsets_(std::move(c.current_rec_binding_offsets_)) {
  c.current_rec_binding_offsets_ = new_offsets;
}

rec_bindings_guard::rec_bindings_guard(compiler& c)
    : compiler_(c), saved_offsets_(std::move(c.current_rec_binding_offsets_)) {
  c.current_rec_binding_offsets_.clear();
}

rec_bindings_guard::~rec_bindings_guard() {
  compiler_.current_rec_binding_offsets_ = std::move(saved_offsets_);
}

// =============================================================================
// let_bindings_guard
// =============================================================================

let_bindings_guard::let_bindings_guard(
    compiler& c, const std::unordered_map<std::uint32_t, std::uint32_t>& new_offsets)
    : compiler_(c), saved_offsets_(std::move(c.current_let_binding_offsets_)) {
  c.current_let_binding_offsets_ = new_offsets;
}

let_bindings_guard::let_bindings_guard(compiler& c)
    : compiler_(c), saved_offsets_(std::move(c.current_let_binding_offsets_)) {
  c.current_let_binding_offsets_.clear();
}

let_bindings_guard::~let_bindings_guard() {
  compiler_.current_let_binding_offsets_ = std::move(saved_offsets_);
}

// =============================================================================
// with_scope_guard
// =============================================================================

with_scope_guard::with_scope_guard(compiler& c, std::uint32_t namespace_local_index)
    : compiler_(c) {
  c.with_scopes_.push_back({namespace_local_index});
}

with_scope_guard::~with_scope_guard() {
  compiler_.with_scopes_.pop_back();
}

// =============================================================================
// compilation_context_guard
// =============================================================================

compilation_context_guard::compilation_context_guard(compiler& c, lexical_scope& new_scope)
    : scope_(c, new_scope), lambda_(c), rec_(std::nullopt) {}

compilation_context_guard::compilation_context_guard(
    compiler& c, lexical_scope& new_scope,
    const std::unordered_map<std::uint32_t, std::uint32_t>& rec_offsets)
    : scope_(c, new_scope), lambda_(c), rec_(std::in_place, c, rec_offsets) {}

} // namespace straylight::nix::compiler::compile
