#pragma once
///@file

#include "nix/util/logging.h"

namespace nix {

std::unique_ptr<logger_t> make_progress_bar();

}
