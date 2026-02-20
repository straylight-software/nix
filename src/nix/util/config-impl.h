#pragma once
/**
 * @file
 *
 * Template implementations (as opposed to mere declarations).
 *
 * This file is an example of the "impl.h" pattern. See the
 * contributing guide.
 *
 * One only needs to include this when one is declaring a
 * `BaseClass<CustomType>` setting, or as derived class of such an
 * instantiation.
 */

#include "nix/util/args.h"
#include "nix/util/configuration.h"
#include "nix/util/file-path.h"
#include "nix/util/logging.h"
#include "nix/util/util.h"

namespace nix {

template <>
struct base_setting_t<strings_t>::trait {
  static constexpr bool appendable = true;
};

template <>
struct base_setting_t<string_set_t>::trait {
  static constexpr bool appendable = true;
};

template <>
struct base_setting_t<string_map_t>::trait {
  static constexpr bool appendable = true;
};

template <>
struct base_setting_t<std::set<experimental_feature_t>>::trait {
  static constexpr bool appendable = true;
};

template <typename T>
struct base_setting_t<T>::trait {
  static constexpr bool appendable = false;
};

template <typename T>
bool base_setting_t<T>::is_appendable() {
  return trait::appendable;
}

template <>
void base_setting_t<strings_t>::append_or_set(strings_t new_value, bool append);
template <>
void base_setting_t<string_set_t>::append_or_set(string_set_t new_value, bool append);
template <>
void base_setting_t<string_map_t>::append_or_set(string_map_t new_value, bool append);
template <>
void base_setting_t<std::set<experimental_feature_t>>::append_or_set(
    std::set<experimental_feature_t> new_value, bool append);

template <typename T>
void base_setting_t<T>::append_or_set(T new_value, bool append) {
  static_assert(!trait::appendable,
                "using default `appendOrSet` implementation with an appendable type");
  assert(!append);

  value_ = std::move(new_value);
}

template <typename T>
void base_setting_t<T>::set(const std::string& str, bool append) {
  if (experimental_feature_settings.is_enabled(experimental_feature))
    append_or_set(parse(str), append);
  else {
    assert(experimental_feature);
    warn("Ignoring setting '%s' because experimental feature '%s' is not enabled", name,
         show_experimental_feature(*experimental_feature));
  }
}

template <>
void base_setting_t<bool>::convert_to_arg(Args& args, const std::string& category);

template <typename T>
void base_setting_t<T>::convert_to_arg(Args& args, const std::string& category) {
  args.add_flag({
      .long_name = name,
      .aliases = aliases,
      .description = fmt("Set the `%s` setting.", name),
      .category = category,
      .labels = {"value"},
      .handler = {[this](std::string s) {
        overridden = true;
        set(s);
      }},
      .experimental_feature = experimental_feature,
  });

  if (is_appendable())
    args.add_flag({
        .long_name = "extra-" + name,
        .aliases = aliases,
        .description = fmt("Append to the `%s` setting.", name),
        .category = category,
        .labels = {"value"},
        .handler = {[this](std::string s) {
          overridden = true;
          set(s, true);
        }},
        .experimental_feature = experimental_feature,
    });
}

#define DECLARE_CONFIG_SERIALISER(TY)                                                              \
  template <>                                                                                      \
  TY base_setting_t<TY>::parse(const std::string& str) const;                                      \
  template <>                                                                                      \
  std::string base_setting_t<TY>::to_string() const;

DECLARE_CONFIG_SERIALISER(std::string)
DECLARE_CONFIG_SERIALISER(std::optional<std::string>)
DECLARE_CONFIG_SERIALISER(bool)
DECLARE_CONFIG_SERIALISER(strings_t)
DECLARE_CONFIG_SERIALISER(string_set_t)
DECLARE_CONFIG_SERIALISER(string_map_t)
DECLARE_CONFIG_SERIALISER(std::set<experimental_feature_t>)
DECLARE_CONFIG_SERIALISER(std::filesystem::path)
DECLARE_CONFIG_SERIALISER(std::optional<std::filesystem::path>)

template <typename T>
T base_setting_t<T>::parse(const std::string& str) const {
  static_assert(std::is_integral<T>::value, "Integer required.");

  try {
    return string2_int_with_unit_prefix<T>(str);
  } catch (...) {
    throw UsageError("setting '%s' has invalid value '%s'", name, str);
  }
}

template <typename T>
std::string base_setting_t<T>::to_string() const {
  static_assert(std::is_integral<T>::value, "Integer required.");

  return std::to_string(value_);
}

} // namespace nix
