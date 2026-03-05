/**
 * @file internal.h
 * @brief Internal definitions for Nix FFI implementation.
 *
 * This header is NOT part of the public API.
 */

#ifndef NIX_FFI_INTERNAL_H
#define NIX_FFI_INTERNAL_H

#include <memory>
#include <mutex>
#include <string>

#include <nix/expr/eval-error.h>
#include <nix/expr/eval.h>
#include <nix/expr/nixexpr.h>
#include <nix/expr/search-path.h>
#include <nix/expr/value.h>
#include <nix/fetchers/fetch-settings.h>
#include <nix/store/derivations.h>
#include <nix/store/path.h>
#include <nix/store/store-api.h>

#include "nix.h"

/* ═══════════════════════════════════════════════════════════════════════════
 * Handle Wrapper Definitions (global namespace to match nix.h forward decls)
 * ═══════════════════════════════════════════════════════════════════════════ */

/**
 * Store handle wrapper.
 */
struct NixStore {
  std::shared_ptr<nix::store_t> store;
};

/**
 * Eval state wrapper.
 */
struct NixEvalState {
  std::shared_ptr<nix::eval_state_t> state;
  NixStore* store_ref; // Keep reference to store
};

/**
 * Expression wrapper.
 */
struct NixExpr {
  nix::expr_t* expr; // Owned by eval state's allocator
  NixEvalState* state_ref;
};

/**
 * Value wrapper.
 * Values are GC-managed by the eval state, so we need to root them.
 */
struct NixValue {
  nix::value_t* value; // GC-rooted
  NixEvalState* state_ref;
};

/**
 * Store path wrapper.
 */
struct NixStorePath {
  nix::store_path_t path;
};

/**
 * Derivation wrapper.
 */
struct NixDerivation {
  nix::derivation_t drv;
  std::string name;
};

/**
 * Position wrapper.
 */
struct NixPos {
  nix::pos_t pos;
};

/**
 * Symbol wrapper.
 */
struct NixSymbol {
  nix::symbol_t sym;
};

/* ═══════════════════════════════════════════════════════════════════════════
 * Implementation Helpers (in namespace)
 * ═══════════════════════════════════════════════════════════════════════════ */

namespace nix::ffi {

/* ═══════════════════════════════════════════════════════════════════════════
 * Error Handling
 * ═══════════════════════════════════════════════════════════════════════════ */

/**
 * Thread-local error state.
 */
struct ErrorState {
  NixError code = NIX_OK;
  std::string message;
};

/**
 * Get thread-local error state.
 */
ErrorState& get_error_state();

/**
 * Set error state from C++ exception.
 */
NixError set_error(NixError code, const char* msg);
NixError set_error(NixError code, const std::string& msg);
NixError set_error_from_exception(const std::exception& e);

/**
 * Catch exceptions and convert to error codes.
 * Usage: return catch_errors([&] { ... return NIX_OK; });
 */
template <typename F>
NixError catch_errors(F&& f) noexcept {
  try {
    return f();
  } catch (const nix::EvalError& e) {
    return set_error(NIX_ERR_EVAL, e.what());
  } catch (const nix::sys_error_t& e) {
    return set_error(NIX_ERR_IO, e.what());
  } catch (const nix::Error& e) {
    return set_error(NIX_ERR_INTERNAL, e.what());
  } catch (const std::bad_alloc&) {
    return set_error(NIX_ERR_INTERNAL, "out of memory");
  } catch (const std::exception& e) {
    return set_error(NIX_ERR_INTERNAL, e.what());
  } catch (...) {
    return set_error(NIX_ERR_INTERNAL, "unknown exception");
  }
}

/* ═══════════════════════════════════════════════════════════════════════════
 * String Helpers
 * ═══════════════════════════════════════════════════════════════════════════ */

/**
 * Set NixString from std::string (copies, caller must free).
 */
void string_set(NixString* out, const std::string& s);

/**
 * Set NixString from static string (no copy, no free needed).
 */
void string_set_static(NixString* out, const char* s);

/* ═══════════════════════════════════════════════════════════════════════════
 * Logging
 * ═══════════════════════════════════════════════════════════════════════════ */

struct LogState {
  NixLogCallback callback = nullptr;
  void* user_data = nullptr;
  NixLogLevel min_level = NIX_LOG_WARN;
  std::mutex mutex;
};

LogState& get_log_state();

} // namespace nix::ffi

#endif // NIX_FFI_INTERNAL_H
