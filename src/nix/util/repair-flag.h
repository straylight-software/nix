#pragma once

///@file

namespace nix {

enum repair_flag : bool { no_repair = false, repair = true };

// PascalCase aliases for backward compatibility
using RepairFlag = repair_flag;
constexpr repair_flag NoRepair = no_repair;
constexpr repair_flag Repair = repair;

} // namespace nix
