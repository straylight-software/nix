/**
 * @file error.cpp
 * @brief Error handling implementation for Nix FFI.
 */

#include <cstring>

#include "internal.h"

namespace nix::ffi {

static thread_local ErrorState tl_error_state;

ErrorState& get_error_state() {
  return tl_error_state;
}

NixError set_error(NixError code, const char* msg) {
  auto& state = get_error_state();
  state.code = code;
  state.message = msg ? msg : "";
  return code;
}

NixError set_error(NixError code, const std::string& msg) {
  auto& state = get_error_state();
  state.code = code;
  state.message = msg;
  return code;
}

NixError set_error_from_exception(const std::exception& e) {
  return set_error(NIX_ERR_INTERNAL, e.what());
}

void string_set(NixString* out, const std::string& s) {
  if (!out) {
    return;
  }
  char* buf = new char[s.size() + 1];
  std::memcpy(buf, s.data(), s.size());
  buf[s.size()] = '\0';
  out->data = buf;
  out->len = s.size();
  out->owned = true;
}

void string_set_static(NixString* out, const char* s) {
  if (!out) {
    return;
  }
  out->data = s;
  out->len = s ? std::strlen(s) : 0;
  out->owned = false;
}

static LogState g_log_state;

LogState& get_log_state() {
  return g_log_state;
}

} // namespace nix::ffi

extern "C" {

const char* nix_error_name(NixError error) {
  switch (error) {
    case NIX_OK:
      return "NIX_OK";
    case NIX_ERR_INVALID_ARG:
      return "NIX_ERR_INVALID_ARG";
    case NIX_ERR_NULL_POINTER:
      return "NIX_ERR_NULL_POINTER";
    case NIX_ERR_PARSE:
      return "NIX_ERR_PARSE";
    case NIX_ERR_EVAL:
      return "NIX_ERR_EVAL";
    case NIX_ERR_TYPE:
      return "NIX_ERR_TYPE";
    case NIX_ERR_STORE:
      return "NIX_ERR_STORE";
    case NIX_ERR_BUILD:
      return "NIX_ERR_BUILD";
    case NIX_ERR_PATH:
      return "NIX_ERR_PATH";
    case NIX_ERR_IO:
      return "NIX_ERR_IO";
    case NIX_ERR_INTERRUPTED:
      return "NIX_ERR_INTERRUPTED";
    case NIX_ERR_OVERFLOW:
      return "NIX_ERR_OVERFLOW";
    case NIX_ERR_NOT_FOUND:
      return "NIX_ERR_NOT_FOUND";
    case NIX_ERR_INTERNAL:
      return "NIX_ERR_INTERNAL";
    default:
      return "NIX_ERR_UNKNOWN";
  }
}

const char* nix_error_message(void) {
  auto& state = nix::ffi::get_error_state();
  return state.message.empty() ? nullptr : state.message.c_str();
}

void nix_error_clear(void) {
  auto& state = nix::ffi::get_error_state();
  state.code = NIX_OK;
  state.message.clear();
}

void nix_string_free(NixString* str) {
  if (str && str->owned && str->data) {
    delete[] str->data;
    str->data = nullptr;
    str->len = 0;
    str->owned = false;
  }
}

void nix_string_array_free(NixString* strings, size_t count) {
  if (!strings) {
    return;
  }
  for (size_t i = 0; i < count; ++i) {
    nix_string_free(&strings[i]);
  }
  delete[] strings;
}

const char* nix_value_type_name(NixValueType type) {
  switch (type) {
    case NIX_TYPE_THUNK:
      return "thunk";
    case NIX_TYPE_INT:
      return "int";
    case NIX_TYPE_FLOAT:
      return "float";
    case NIX_TYPE_BOOL:
      return "bool";
    case NIX_TYPE_STRING:
      return "string";
    case NIX_TYPE_PATH:
      return "path";
    case NIX_TYPE_NULL:
      return "null";
    case NIX_TYPE_ATTRS:
      return "attrs";
    case NIX_TYPE_LIST:
      return "list";
    case NIX_TYPE_FUNCTION:
      return "function";
    case NIX_TYPE_EXTERNAL:
      return "external";
    case NIX_TYPE_FAILED:
      return "failed";
    default:
      return "unknown";
  }
}

void nix_set_log_callback(NixLogCallback callback, void* user_data) {
  auto& state = nix::ffi::get_log_state();
  std::lock_guard<std::mutex> lock(state.mutex);
  state.callback = callback;
  state.user_data = user_data;
}

void nix_set_log_level(NixLogLevel level) {
  auto& state = nix::ffi::get_log_state();
  std::lock_guard<std::mutex> lock(state.mutex);
  state.min_level = level;
}

} // extern "C"
