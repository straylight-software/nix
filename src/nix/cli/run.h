#pragma once
///@file

#include "nix/store/store-api.h"

namespace nix {

enum struct use_lookup_path_t { use, dont_use };

void exec_program_in_store(ref<Store> store, use_lookup_path_t use_lookup_path, const std::string& program,
                        const strings_t& args, std::optional<std::string_view> system = std::nullopt,
                        std::optional<string_map_t> env = std::nullopt);

} // namespace nix
