#pragma once
/// @file straylight/nix/compiler/log.h
/// Structured logging for the Nix compiler.
///
/// Uses spdlog with std::format. All logging goes through this header to ensure
/// consistent formatting and allow centralized configuration.
///
/// Log Levels:
/// - trace: Extremely verbose, for tracing execution flow
/// - debug: Debugging info, disabled in release builds
/// - info:  Normal operational messages
/// - warn:  Warning conditions
/// - error: Error conditions
/// - critical: Critical failures
///
/// Usage:
/// @code
///   #include "straylight/nix/compiler/log.h"
///
///   LOG_DEBUG("compiling expression at line {}", line);
///   LOG_TRACE("forcing thunk_ptr={} module={}", ptr, mod);
///   LOG_ERROR("infinite recursion detected in thunk {}", id);
/// @endcode
///
/// Configuration:
/// - Set SPDLOG_ACTIVE_LEVEL before including to control compile-time filtering
/// - Call compiler::log::init() to configure runtime settings
/// - Environment: STRAYLIGHT_LOG_LEVEL=trace|debug|info|warn|error|critical

#include <string_view>

// Compile-time log level filtering
// Define SPDLOG_ACTIVE_LEVEL before including spdlog.h to enable/disable levels
// at compile time. Levels below this are completely compiled out.
#ifndef SPDLOG_ACTIVE_LEVEL
#  ifdef NDEBUG
#    define SPDLOG_ACTIVE_LEVEL SPDLOG_LEVEL_INFO
#  else
#    define SPDLOG_ACTIVE_LEVEL SPDLOG_LEVEL_TRACE
#  endif
#endif

#include <spdlog/spdlog.h>

namespace straylight::nix::compiler::log {

/// Logger name for the compiler subsystem
inline constexpr std::string_view logger_name = "nix-compiler";

/// Initialize the logging system.
/// Call once at startup. Reads STRAYLIGHT_LOG_LEVEL from environment.
inline void init() {
  // Check environment for log level override
  if (const char* level_env = std::getenv("STRAYLIGHT_LOG_LEVEL")) {
    std::string_view level{level_env};
    if (level == "trace") {
      spdlog::set_level(spdlog::level::trace);
    } else if (level == "debug") {
      spdlog::set_level(spdlog::level::debug);
    } else if (level == "info") {
      spdlog::set_level(spdlog::level::info);
    } else if (level == "warn") {
      spdlog::set_level(spdlog::level::warn);
    } else if (level == "error") {
      spdlog::set_level(spdlog::level::err);
    } else if (level == "critical") {
      spdlog::set_level(spdlog::level::critical);
    } else if (level == "off") {
      spdlog::set_level(spdlog::level::off);
    }
  }

  // Set pattern: [timestamp] [level] [logger] message
  spdlog::set_pattern("[%Y-%m-%d %H:%M:%S.%e] [%^%l%$] %v");
}

/// Set the log level programmatically
inline void set_level(spdlog::level::level_enum level) {
  spdlog::set_level(level);
}

/// Get the current log level
[[nodiscard]] inline auto get_level() -> spdlog::level::level_enum {
  return spdlog::get_level();
}

} // namespace straylight::nix::compiler::log

// Convenience macros that include source location
// These use spdlog's SPDLOG_* macros which respect SPDLOG_ACTIVE_LEVEL

#define LOG_TRACE(...) SPDLOG_TRACE(__VA_ARGS__)
#define LOG_DEBUG(...) SPDLOG_DEBUG(__VA_ARGS__)
#define LOG_INFO(...) SPDLOG_INFO(__VA_ARGS__)
#define LOG_WARN(...) SPDLOG_WARN(__VA_ARGS__)
#define LOG_ERROR(...) SPDLOG_ERROR(__VA_ARGS__)
#define LOG_CRITICAL(...) SPDLOG_CRITICAL(__VA_ARGS__)

// Conditional logging (runtime check)
#define LOG_TRACE_IF(cond, ...)                                                                    \
  do {                                                                                             \
    if (cond)                                                                                      \
      SPDLOG_TRACE(__VA_ARGS__);                                                                   \
  } while (0)
#define LOG_DEBUG_IF(cond, ...)                                                                    \
  do {                                                                                             \
    if (cond)                                                                                      \
      SPDLOG_DEBUG(__VA_ARGS__);                                                                   \
  } while (0)
#define LOG_INFO_IF(cond, ...)                                                                     \
  do {                                                                                             \
    if (cond)                                                                                      \
      SPDLOG_INFO(__VA_ARGS__);                                                                    \
  } while (0)
#define LOG_WARN_IF(cond, ...)                                                                     \
  do {                                                                                             \
    if (cond)                                                                                      \
      SPDLOG_WARN(__VA_ARGS__);                                                                    \
  } while (0)
#define LOG_ERROR_IF(cond, ...)                                                                    \
  do {                                                                                             \
    if (cond)                                                                                      \
      SPDLOG_ERROR(__VA_ARGS__);                                                                   \
  } while (0)
