#include "nix/fetchers/attrs.h"

#include <nlohmann/json.hpp>

#include "nix/fetchers/fetchers.h"

namespace nix::fetchers {

Attrs json_to_attrs(const nlohmann::json& json) {
  Attrs attrs;

  for (auto& i : json.items()) {
    if (i.value().is_number()) {
      attrs.emplace(i.key(), i.value().get<uint64_t>());
    } else if (i.value().is_string()) {
      attrs.emplace(i.key(), i.value().get<std::string>());
    } else if (i.value().is_boolean()) {
      attrs.emplace(i.key(), explicit_t<bool>{i.value().get<bool>()});
    } else {
      throw Error("unsupported input attribute type in lock file");
    }
  }

  return attrs;
}

nlohmann::json attrs_to_json(const Attrs& attrs) {
  nlohmann::json json;
  for (auto& attr : attrs) {
    if (auto v = std::get_if<uint64_t>(&attr.second)) {
      json[attr.first] = *v;
    } else if (auto v = std::get_if<std::string>(&attr.second)) {
      json[attr.first] = *v;
    } else if (auto v = std::get_if<explicit_t<bool>>(&attr.second)) {
      json[attr.first] = v->t_;
    } else {
      unreachable();
    }
  }
  return json;
}

std::optional<std::string> maybe_get_str_attr(const Attrs& attrs, const std::string& name) {
  auto i = attrs.find(name);
  if (i == attrs.end()) {
    return {};
  }
  if (auto v = std::get_if<std::string>(&i->second)) {
    return *v;
  }
  throw Error("input attribute '%s' is not a string %s", name, attrs_to_json(attrs).dump());
}

std::string get_str_attr(const Attrs& attrs, const std::string& name) {
  auto s = maybe_get_str_attr(attrs, name);
  if (!s) {
    throw Error("input attribute '%s' is missing", name);
  }
  return *s;
}

std::optional<uint64_t> maybe_get_int_attr(const Attrs& attrs, const std::string& name) {
  auto i = attrs.find(name);
  if (i == attrs.end()) {
    return {};
  }
  if (auto v = std::get_if<uint64_t>(&i->second)) {
    return *v;
  }
  throw Error("input attribute '%s' is not an integer", name);
}

uint64_t get_int_attr(const Attrs& attrs, const std::string& name) {
  auto s = maybe_get_int_attr(attrs, name);
  if (!s) {
    throw Error("input attribute '%s' is missing", name);
  }
  return *s;
}

std::optional<bool> maybe_get_bool_attr(const Attrs& attrs, const std::string& name) {
  auto i = attrs.find(name);
  if (i == attrs.end()) {
    return {};
  }
  if (auto v = std::get_if<explicit_t<bool>>(&i->second)) {
    return v->t_;
  }
  throw Error("input attribute '%s' is not a Boolean", name);
}

bool get_bool_attr(const Attrs& attrs, const std::string& name) {
  auto s = maybe_get_bool_attr(attrs, name);
  if (!s) {
    throw Error("input attribute '%s' is missing", name);
  }
  return *s;
}

string_map_t attrs_to_query(const Attrs& attrs) {
  string_map_t query;
  for (auto& attr : attrs) {
    if (auto v = std::get_if<uint64_t>(&attr.second)) {
      query.insert_or_assign(attr.first, fmt("%d", *v));
    } else if (auto v = std::get_if<std::string>(&attr.second)) {
      query.insert_or_assign(attr.first, *v);
    } else if (auto v = std::get_if<explicit_t<bool>>(&attr.second)) {
      query.insert_or_assign(attr.first, v->t_ ? "1" : "0");
    } else {
      unreachable();
    }
  }
  return query;
}

Hash get_rev_attr(const Attrs& attrs, const std::string& name) {
  return Hash::parse_any(get_str_attr(attrs, name), hash_algorithm_t::SHA1);
}

} // namespace nix::fetchers
