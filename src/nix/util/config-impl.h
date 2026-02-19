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
bool base_setting_t<T>::isAppendable() {
  return trait::appendable;
}

template <>
void base_setting_t<strings_t>::appendOrSet(strings_t newValue, bool append);
template <>
void base_setting_t<string_set_t>::appendOrSet(string_set_t newValue, bool append);
template <>
void base_setting_t<string_map_t>::appendOrSet(string_map_t newValue, bool append);
template <>
void base_setting_t<std::set<experimental_feature_t>>::appendOrSet(std::set<experimental_feature_t> newValue,
                                                             bool append);

template <typename T>
void base_setting_t<T>::appendOrSet(T newValue, bool append) {
  static_assert(!trait::appendable,
                "using default `appendOrSet` implementation with an appendable type");
  assert(!append);

  value = std::move(newValue);
}

template <typename T>
void base_setting_t<T>::set(const std::string& str, bool append) {
  if (experimentalFeatureSettings.isEnabled(experimentalFeature))
    appendOrSet(parse(str), append);
  else {
    assert(experimentalFeature);
    warn("Ignoring setting '%s' because experimental feature '%s' is not enabled", name,
         showExperimentalFeature(*experimentalFeature));
  }
}

template <>
void base_setting_t<bool>::convertToArg(Args& args, const std::string& category);

template <typename T>
void base_setting_t<T>::convertToArg(Args& args, const std::string& category) {
  args.addFlag({
      .longName = name,
      .aliases = aliases,
      .description = fmt("Set the `%s` setting.", name),
      .category = category,
      .labels = {"value"},
      .handler = {[this](std::string s) {
        overridden = true;
        set(s);
      }},
      .experimentalFeature = experimentalFeature,
  });

  if (isAppendable())
    args.addFlag({
        .longName = "extra-" + name,
        .aliases = aliases,
        .description = fmt("Append to the `%s` setting.", name),
        .category = category,
        .labels = {"value"},
        .handler = {[this](std::string s) {
          overridden = true;
          set(s, true);
        }},
        .experimentalFeature = experimentalFeature,
    });
}

#define DECLARE_CONFIG_SERIALISER(TY)                                                              \
  template <>                                                                                      \
  TY base_setting_t<TY>::parse(const std::string& str) const;                                         \
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
    return string2IntWithUnitPrefix<T>(str);
  } catch (...) {
    throw UsageError("setting '%s' has invalid value '%s'", name, str);
  }
}

template <typename T>
std::string base_setting_t<T>::to_string() const {
  static_assert(std::is_integral<T>::value, "Integer required.");

  return std::to_string(value);
}

} // namespace nix
