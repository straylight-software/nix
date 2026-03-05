/**
 * @file eval.cpp
 * @brief Evaluator operations for Nix FFI.
 */

#include <sstream>

#include "internal.h"

// Global settings for FFI use - initialized on first use
namespace {

nix::fetchers::settings_t& get_ffi_fetch_settings() {
  static nix::fetchers::settings_t settings;
  return settings;
}

// We need a bool for read-only mode
static bool g_ffi_read_only_mode = false;

nix::eval_settings_t& get_ffi_eval_settings() {
  static nix::eval_settings_t settings(g_ffi_read_only_mode);
  return settings;
}

} // namespace

extern "C" {

NixError nix_eval_state_new(NixStore* store, const NixEvalConfig* config,
                            NixEvalState** state_out) {
  if (!store || !state_out) {
    return nix::ffi::set_error(NIX_ERR_NULL_POINTER, "null argument");
  }

  return nix::ffi::catch_errors([&] {
    nix::LookupPath lookup_path;
    if (config && config->lookup_path) {
      for (size_t i = 0; i < config->lookup_path_count; ++i) {
        lookup_path.elements.push_back(nix::LookupPath::Elem::parse(config->lookup_path[i]));
      }
    }

    // ref<T> is a non-nullable wrapper around shared_ptr<T>
    nix::ref<nix::store_t> store_ref(store->store);

    auto state = std::make_shared<nix::eval_state_t>(
        lookup_path, store_ref, get_ffi_fetch_settings(), get_ffi_eval_settings());

    auto* handle = new NixEvalState{std::move(state), store};
    *state_out = handle;
    return NIX_OK;
  });
}

void nix_eval_state_free(NixEvalState* state) {
  delete state;
}

// Note: nix_eval_state_add_lookup_path was removed because lookup_path is private
// in eval_state_t after construction. Lookup paths must be provided via
// NixEvalConfig at eval state creation time.

NixError nix_parse_expr_string(NixEvalState* state, const char* expr_str, const char* base_path,
                               NixExpr** expr_out) {
  if (!state || !expr_str || !expr_out) {
    return nix::ffi::set_error(NIX_ERR_NULL_POINTER, "null argument");
  }

  return nix::ffi::catch_errors([&] {
    auto base = base_path ? state->state->root_path(base_path) : state->state->root_path(".");
    auto* expr = state->state->parseExprFromString(expr_str, base);

    auto* handle = new NixExpr{expr, state};
    *expr_out = handle;
    return NIX_OK;
  });
}

NixError nix_parse_expr_file(NixEvalState* state, const char* file_path, NixExpr** expr_out) {
  if (!state || !file_path || !expr_out) {
    return nix::ffi::set_error(NIX_ERR_NULL_POINTER, "null argument");
  }

  return nix::ffi::catch_errors([&] {
    auto path = state->state->root_path(file_path);
    auto* expr = state->state->parseExprFromFile(path);

    auto* handle = new NixExpr{expr, state};
    *expr_out = handle;
    return NIX_OK;
  });
}

void nix_expr_free(NixExpr* expr) {
  // Note: The actual expr_t is owned by the eval state's allocator
  delete expr;
}

NixError nix_expr_show(const NixExpr* expr, NixString* out) {
  if (!expr || !out) {
    return nix::ffi::set_error(NIX_ERR_NULL_POINTER, "null argument");
  }

  return nix::ffi::catch_errors([&] {
    std::ostringstream ss;
    expr->expr->show(expr->state_ref->state->symbols, ss);
    nix::ffi::string_set(out, ss.str());
    return NIX_OK;
  });
}

NixError nix_eval(NixEvalState* state, NixExpr* expr, NixValue** value_out) {
  if (!state || !expr || !value_out) {
    return nix::ffi::set_error(NIX_ERR_NULL_POINTER, "null argument");
  }

  return nix::ffi::catch_errors([&] {
    auto* v = state->state->allocValue();
    state->state->eval(expr->expr, *v);

    auto* handle = new NixValue{v, state};
    *value_out = handle;
    return NIX_OK;
  });
}

NixError nix_eval_string(NixEvalState* state, const char* expr_str, const char* base_path,
                         NixValue** value_out) {
  NixExpr* expr = nullptr;
  NixError err = nix_parse_expr_string(state, expr_str, base_path, &expr);
  if (err != NIX_OK) {
    return err;
  }

  err = nix_eval(state, expr, value_out);
  nix_expr_free(expr);
  return err;
}

NixError nix_eval_file(NixEvalState* state, const char* file_path, NixValue** value_out) {
  NixExpr* expr = nullptr;
  NixError err = nix_parse_expr_file(state, file_path, &expr);
  if (err != NIX_OK) {
    return err;
  }

  err = nix_eval(state, expr, value_out);
  nix_expr_free(expr);
  return err;
}

NixError nix_value_force(NixEvalState* state, NixValue* value, size_t depth) {
  if (!state || !value) {
    return nix::ffi::set_error(NIX_ERR_NULL_POINTER, "null argument");
  }

  return nix::ffi::catch_errors([&] {
    if (depth == 0) {
      state->state->forceValue(*value->value, nix::no_pos);
    } else {
      // Force recursively
      state->state->forceValueDeep(*value->value);
    }
    return NIX_OK;
  });
}

void nix_value_free(NixValue* value) {
  // Note: The actual value_t is GC-managed by eval state
  delete value;
}

} // extern "C"
