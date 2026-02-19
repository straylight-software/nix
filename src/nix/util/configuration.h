#pragma once
///@file

#include <cassert>
#include <map>
#include <set>

#include <nlohmann/json_fwd.hpp>

#include "nix/util/experimental-features.h"
#include "nix/util/types.h"

namespace nix {

/**
 * The Config class provides Nix runtime configurations.
 *
 * What is a Configuration?
 *   A collection of uniquely named settings_t.
 *
 * What is a setting_t?
 *   Each property that you can set in a configuration corresponds to a
 *   `setting_t`. A setting records value and description of a property
 *   with a default and optional aliases.
 *
 * A valid configuration consists of settings that are registered to a
 * `Config` object instance:
 *
 *   Config config;
 *   setting_t<std::string> systemSetting{&config, "x86_64-linux", "system", "the current system"};
 *
 * The above creates a `Config` object and registers a setting called "system"
 * via the variable `systemSetting` with it. The setting defaults to the string
 * "x86_64-linux", it's description is "the current system". All of the
 * registered settings can then be accessed as shown below:
 *
 *   std::map<std::string, Config::setting_info_t> settings;
 *   config.getSettings(settings);
 *   settings["system"].description == "the current system"
 *   settings["system"].value == "x86_64-linux"
 *
 *
 * The above retrieves all currently known settings from the `Config` object
 * and adds them to the `settings` map.
 */

class Args;
class abstract_setting_t;

class abstract_config_t {
protected:
  string_map_t unknownSettings;

  abstract_config_t(string_map_t initials = {});

public:
  /**
   * Sets the value referenced by `name` to `value`. Returns true if the
   * setting is known, false otherwise.
   */
  virtual bool set(const std::string& name, const std::string& value) = 0;

  struct setting_info_t {
    std::string value;
    std::string description;
  };

  /**
   * Adds the currently known settings to the given result map `res`.
   * - res: map to store settings in
   * - overriddenOnly: when set to true only overridden settings will be added to `res`
   */
  virtual void getSettings(std::map<std::string, setting_info_t>& res,
                           bool overriddenOnly = false) const = 0;

  /**
   * Parses the configuration in `contents` and applies it
   * - contents: configuration contents to be parsed and applied
   * - path: location of the configuration file
   */
  void applyConfig(const std::string& contents, const std::string& path = "<unknown>");

  /**
   * Resets the `overridden` flag of all settings_t
   */
  virtual void resetOverridden() = 0;

  /**
   * Outputs all settings to JSON
   * - out: JSONObject to write the configuration to
   */
  virtual nlohmann::json toJSON() = 0;

  /**
   * Outputs all settings in a key-value pair format suitable to be used as
   * `nix.conf`
   */
  virtual std::string toKeyValue() = 0;

  /**
   * Converts settings to `Args` to be used on the command line interface
   * - args: args to write to
   * - category: category of the settings
   */
  virtual void convertToArgs(Args& args, const std::string& category) = 0;

  /**
   * Logs a warning for each unregistered setting
   */
  void warnUnknownSettings();

  /**
   * Re-applies all previously attempted changes to unknown settings
   */
  void reapplyUnknownSettings();

  virtual ~abstract_config_t() = default;
};

/**
 * A class to simplify providing configuration settings. The typical
 * use is to inherit Config and add setting_t<T> members:
 *
 * class MyClass : private Config
 * {
 *   setting_t<int> foo{this, 123, "foo", "the number of foos to use"};
 *   setting_t<std::string> bar{this, "blabla", "bar", "the name of the bar"};
 *
 *   MyClass() : Config(readConfigFile("/etc/my-app.conf"))
 *   {
 *     std::cout << foo << "\n"; // will print 123 unless overridden
 *   }
 * };
 */
class Config : public abstract_config_t {
  friend class abstract_setting_t;

public:
  struct setting_data_t {
    bool isAlias;
    abstract_setting_t* setting;
  };

  using settings_t = std::map<std::string, setting_data_t>;

private:
  settings_t _settings;

public:
  Config(string_map_t initials = {});

  bool set(const std::string& name, const std::string& value) override;

  void addSetting(abstract_setting_t* setting);

  void getSettings(std::map<std::string, setting_info_t>& res,
                   bool overriddenOnly = false) const override;

  void resetOverridden() override;

  nlohmann::json toJSON() override;

  std::string toKeyValue() override;

  void convertToArgs(Args& args, const std::string& category) override;
};

class abstract_setting_t {
  friend class Config;

public:
  const std::string name;
  const std::string description;
  const string_set_t aliases;

  int created = 123;

  bool overridden = false;

  std::optional<experimental_feature_t> experimentalFeature;

protected:
  abstract_setting_t(const std::string& name, const std::string& description, const string_set_t& aliases,
                  std::optional<experimental_feature_t> experimentalFeature = std::nullopt);

  virtual ~abstract_setting_t();

  virtual void set(const std::string& value, bool append = false) = 0;

  /**
   * Whether the type is appendable; i.e. whether the `append`
   * parameter to `set()` is allowed to be `true`.
   */
  virtual bool isAppendable() = 0;

  virtual std::string to_string() const = 0;

  nlohmann::json toJSON();

  virtual std::map<std::string, nlohmann::json> toJSONObject() const;

  virtual void convertToArg(Args& args, const std::string& category);

  bool isOverridden() const;
};

/**
 * A setting of type T.
 */
template <typename T>
class base_setting_t : public abstract_setting_t {
protected:
  T value;
  const T defaultValue;
  const bool documentDefault;

  /**
   * Parse the string into a `T`.
   *
   * Used by `set()`.
   */
  virtual T parse(const std::string& str) const;

  /**
   * Append or overwrite `value` with `newValue`.
   *
   * Some types to do not support appending in which case `append`
   * should never be passed. The default handles this case.
   *
   * @param append Whether to append or overwrite.
   */
  virtual void appendOrSet(T newValue, bool append);

public:
  base_setting_t(const T& def, const bool documentDefault, const std::string& name,
              const std::string& description, const string_set_t& aliases = {},
              std::optional<experimental_feature_t> experimentalFeature = std::nullopt)
      : abstract_setting_t(name, description, aliases, experimentalFeature),
        value(def),
        defaultValue(def),
        documentDefault(documentDefault) {}

  operator const T&() const { return value; }

  operator T&() { return value; }

  const T& get() const { return value; }

  T& get() { return value; }

  template <typename U>
  bool operator==(const U& v2) const {
    return value == v2;
  }

  template <typename U>
  bool operator!=(const U& v2) const {
    return value != v2;
  }

  template <typename U>
  void operator=(const U& v) {
    assign(v);
  }

  virtual void assign(const T& v) { value = v; }

  template <typename U>
  void setDefault(const U& v) {
    if (!overridden)
      value = v;
  }

  /**
   * Require any experimental feature the setting depends on
   *
   * Uses `parse()` to get the value from `str`, and `appendOrSet()`
   * to set it.
   */
  void set(const std::string& str, bool append = false) override final;

  /**
   * C++ trick; This is template-specialized to compile-time indicate whether
   * the type is appendable.
   */
  struct trait;

  /**
   * Always defined based on the C++ magic
   * with `trait` above.
   */
  bool isAppendable() override final;

  virtual void override(const T& v) {
    overridden = true;
    value = v;
  }

  std::string to_string() const override;

  void convertToArg(Args& args, const std::string& category) override;

  std::map<std::string, nlohmann::json> toJSONObject() const override;
};

template <typename T>
std::ostream& operator<<(std::ostream& str, const base_setting_t<T>& opt) {
  return str << static_cast<const T&>(opt);
}

template <typename T>
bool operator==(const T& v1, const base_setting_t<T>& v2) {
  return v1 == static_cast<const T&>(v2);
}

template <typename T>
class setting_t : public base_setting_t<T> {
public:
  setting_t(Config* options, const T& def, const std::string& name, const std::string& description,
          const string_set_t& aliases = {}, const bool documentDefault = true,
          std::optional<experimental_feature_t> experimentalFeature = std::nullopt)
      : base_setting_t<T>(def, documentDefault, name, description, aliases,
                       std::move(experimentalFeature)) {
    options->addSetting(this);
  }

  void operator=(const T& v) { this->assign(v); }
};

/**
 * A special setting for Paths. These are automatically canonicalised
 * (e.g. "/foo//bar/" becomes "/foo/bar").
 *
 * It is mandatory to specify a path; i.e. the empty string is not
 * permitted.
 */
class path_setting_t : public base_setting_t<Path> {
public:
  path_setting_t(Config* options, const Path& def, const std::string& name,
              const std::string& description, const string_set_t& aliases = {});

  Path parse(const std::string& str) const override;

  Path operator+(const char* p) const { return value + p; }

  void operator=(const Path& v) { this->assign(v); }
};

/**
 * Like `path_setting_t`, but the absence of a path is also allowed.
 *
 * `std::optional` is used instead of the empty string for clarity.
 */
class optional_path_setting_t : public base_setting_t<std::optional<Path>> {
public:
  optional_path_setting_t(Config* options, const std::optional<Path>& def, const std::string& name,
                      const std::string& description, const string_set_t& aliases = {});

  std::optional<Path> parse(const std::string& str) const override;

  void operator=(const std::optional<Path>& v);
};

struct experimental_feature_settings_t : Config {
  setting_t<std::set<experimental_feature_t>> experimentalFeatures{this,
                                                              {},
                                                              "experimental-features",
                                                              R"(
          Experimental features that are enabled.

          Example:

          ```
          experimental-features = ca-derivations
          ```

          The following experimental features are available:

          {{#include experimental-features-shortlist.md}}

          Experimental features are [further documented in the manual](@docroot@/development/experimental-features.md).
        )"};

  /**
   * Check whether the given experimental feature is enabled.
   */
  bool isEnabled(const experimental_feature_t&) const;

  /**
   * Require an experimental feature be enabled, throwing an error if it is
   * not.
   */
  void require(const experimental_feature_t&, std::string reason = "") const;

  /**
   * Require an experimental feature be enabled, throwing an error if it is
   * not. The reason is lazily evaluated only if the feature is disabled.
   */
  template <typename GetReason>
    requires std::invocable<GetReason> &&
             std::convertible_to<std::invoke_result_t<GetReason>, std::string>
  void require(const experimental_feature_t& feature, GetReason&& getReason) const {
    if (isEnabled(feature))
      return;
    require(feature, getReason());
  }

  /**
   * `std::nullopt` pointer means no feature, which means there is nothing that could be
   * disabled, and so the function returns true in that case.
   */
  bool isEnabled(const std::optional<experimental_feature_t>&) const;

  /**
   * `std::nullopt` pointer means no feature, which means there is nothing that could be
   * disabled, and so the function does nothing in that case.
   */
  void require(const std::optional<experimental_feature_t>&) const;
};

// FIXME: don't use a global variable.
extern experimental_feature_settings_t experimentalFeatureSettings;

} // namespace nix
