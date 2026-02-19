#pragma once
///@file

#include <optional>
#include <variant>

#include <nlohmann/json_fwd.hpp>

#include "nix/util/hash.h"
#include "nix/util/types.h"

namespace nix::fetchers {

using Attr = std::variant<std::string, uint64_t, Explicit<bool>>;

/**
 * An `Attrs` can be thought of a JSON object restricted or simplified
 * to be "flat", not containing any subcontainers (arrays or objects)
 * and also not containing any `null`s.
 */
using Attrs = std::map<std::string, Attr>;

Attrs json_to_attrs(const nlohmann::json& json);

nlohmann::json attrs_to_json(const Attrs& attrs);

std::optional<std::string> maybe_get_str_attr(const Attrs& attrs, const std::string& name);

std::string get_str_attr(const Attrs& attrs, const std::string& name);

std::optional<uint64_t> maybe_get_int_attr(const Attrs& attrs, const std::string& name);

uint64_t get_int_attr(const Attrs& attrs, const std::string& name);

std::optional<bool> maybe_get_bool_attr(const Attrs& attrs, const std::string& name);

bool get_bool_attr(const Attrs& attrs, const std::string& name);

string_map_t attrs_to_query(const Attrs& attrs);

Hash get_rev_attr(const Attrs& attrs, const std::string& name);

} // namespace nix::fetchers
