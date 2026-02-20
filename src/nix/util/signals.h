#pragma once
///@file

#include <functional>

#include "nix/util/error.h"
#include "nix/util/logging.h"
#include "nix/util/types.h"

namespace nix {

/* User interruption. */

/**
 * @note Does nothing on Windows
 */
static inline void set_interrupted(bool is_interrupted);

/**
 * @note Does nothing on Windows
 */
static inline auto get_interrupted() -> bool;

/**
 * @note Does nothing on Windows
 */
static inline auto is_interrupted() -> bool;

/**
 * @note Does nothing on Windows
 */
inline void check_interrupt();

/**
 * @note Never will happen on Windows
 */
make_error(Interrupted, base_error_t);

struct interrupt_callback_t {
  virtual ~interrupt_callback_t() {}
};

/**
 * Register a function that gets called on SIGINT (in a non-signal
 * context).
 *
 * @note Does nothing on Windows
 */
std::unique_ptr<interrupt_callback_t> create_interrupt_callback(std::function<void()> callback);

/**
 * A RAII class that causes the current thread to receive SIGUSR1 when
 * the signal handler thread receives SIGINT. That is, this allows
 * SIGINT to be multiplexed to multiple threads.
 *
 * @note Does nothing on Windows
 */
struct receive_interrupts_t;

} // namespace nix

#include "nix/util/signals-impl.h"
