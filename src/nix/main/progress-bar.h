#pragma once
///@file

#include "nix/util/logging.h"

namespace nix {

std::unique_ptr<Logger> makeProgressBar();

}
