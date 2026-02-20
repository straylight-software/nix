#pragma once
///@file

// not used, but will be used by callers
#include <variant>

/**
 * Force the default versions of all constructors (copy, move, copy
 * assignment).
 */
#define FORCE_DEFAULT_CONSTRUCTORS(CLASS_NAME)                                                     \
  CLASS_NAME(const CLASS_NAME&) = default;                                                         \
  CLASS_NAME(CLASS_NAME&) = default;                                                               \
  CLASS_NAME(CLASS_NAME&&) = default;                                                              \
                                                                                                   \
  CLASS_NAME& operator=(const CLASS_NAME&) = default;                                              \
  CLASS_NAME& operator=(CLASS_NAME&) = default;

/**
 * Make a wrapper constructor. All args are forwarded to the
 * construction of the "raw" field. (Which we assume is the only one.)
 *
 * The moral equivalent of `using raw_t::raw_t;`
 */
#define MAKE_WRAPPER_CONSTRUCTOR(CLASS_NAME)                                                       \
  FORCE_DEFAULT_CONSTRUCTORS(CLASS_NAME)                                                           \
                                                                                                   \
  template <typename... args_t>                                                                    \
    requires(!(sizeof...(args_t) == 1 &&                                                           \
               (std::is_same_v<std::remove_cvref_t<args_t>, CLASS_NAME> && ...)))                  \
  CLASS_NAME(args_t&&... arg) : raw(std::forward<args_t>(arg)...) {}
