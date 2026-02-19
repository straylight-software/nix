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
 * The config_t class provides Nix runtime configurations.
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
 * `config_t` object instance:
 *
 *   config_t config;
 *   setting_t<std::string> systemSetting{&config, "x86_64-linux", "system", "the current system"};
 *
 * The above creates a `config_t` object and registers a setting called "system"
 * via the variable `systemSetting` with it. The setting defaults to the string
 * "x86_64-linux", it's description is "the current system". All of the
 * registered settings can then be accessed as shown below:
 *
 *   std::map<std::string, config_t::setting_info_t> settings;
 *   config.get_settings(settings);
 *   settings["system"].description == "the current system"
 *   settings["system"].value == "x86_64-linux"
 *
 *
 * The above retrieves all currently known settings from the `config_t` object
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
   * - overridden_only: when set to true only overridden settings will be added to `res`
   */
  virtual void get_settings(std::map<std::string, setting_info_t>& res,
                           bool overridden_only = false) const = 0;

  /**
   * Parses the configuration in `contents` and applies it
   * - contents: configuration contents to be parsed and applied
   * - path: location of the configuration file
   */
  void apply_config(const std::string& contents, const std::string& path = "<unknown>");

  /**
   * Resets the `overridden` flag of all settings_t
   */
  virtual void reset_overridden() = 0;

  /**
   * Outputs all settings to JSON
   * - out: JSONObject to write the configuration to
   */
  virtual nlohmann::json to_json() = 0;

  /**
   * Outputs all settings in a key-value pair format suitable to be used as
   * `nix.conf`
   */
  virtual std::string to_key_value() = 0;

  /**
   * Converts settings to `Args` to be used on the command line interface
   * - args: args to write to
   * - category: category of the settings
   */
  virtual void convert_to_args(Args& args, const std::string& category) = 0;

  /**
   * Logs a warning for each unregistered setting
   */
  void warn_unknown_settings();

  /**
   * Re-applies all previously attempted changes to unknown settings
   */
  void reapply_unknown_settings();

  virtual ~abstract_config_t() = default;
};

/**
 * A class to simplify providing configuration settings. The typical
 * use is to inherit config_t and add setting_t<T> members:
 *
 * class MyClass : private config_t
 * {
 *   setting_t<int> foo{this, 123, "foo", "the number of foos to use"};
 *   setting_t<std::string> bar{this, "blabla", "bar", "the name of the bar"};
 *
 *   MyClass() : config_t(readConfigFile("/etc/my-app.conf"))
 *   {
 *     std::cout << foo << "\n"; // will print 123 unless overridden
 *   }
 * };
 */
class config_t : public abstract_config_t {
  friend class abstract_setting_t;

public:
  struct setting_data_t {
    bool is_alias;
    abstract_setting_t* setting;
  };

  using settings_t = std::map<std::string, setting_data_t>;

private:
  settings_t _settings;

public:
  config_t(string_map_t initials = {});

  bool set(const std::string& name, const std::string& value) override;

  void add_setting(abstract_setting_t* setting);

  void get_settings(std::map<std::string, setting_info_t>& res,
                   bool overridden_only = false) const override;

  void reset_overridden() override;

  nlohmann::json to_json() override;

  std::string to_key_value() override;

  void convert_to_args(Args& args, const std::string& category) override;
};

class abstract_setting_t {
  friend class config_t;

public:
  const std::string name;
  const std::string description;
  const string_set_t aliases;

  int created = 123;

  bool overridden = false;

  std::optional<experimental_feature_t> experimental_feature;

protected:
  abstract_setting_t(const std::string& name, const std::string& description, const string_set_t& aliases,
                  std::optional<experimental_feature_t> experimental_feature = std::nullopt);

  virtual ~abstract_setting_t();

  virtual void set(const std::string& value, bool append = false) = 0;

  /**
   * Whether the type is appendable; i.e. whether the `append`
   * parameter to `set()` is allowed to be `true`.
   */
  virtual bool is_appendable() = 0;

  virtual std::string to_string() const = 0;

  nlohmann::json to_json();

  virtual std::map<std::string, nlohmann::json> to_json_object() const;

  virtual void convert_to_arg(Args& args, const std::string& category);

  bool is_overridden() const;
};

/**
 * A setting of type T.
 */
template <typename T>
class base_setting_t : public abstract_setting_t {
protected:
  T value;
  const T default_value;
  const bool document_default;

  /**
   * Parse the string into a `T`.
   *
   * Used by `set()`.
   */
  virtual T parse(const std::string& str) const;

  /**
   * Append or overwrite `value` with `new_value`.
   *
   * Some types to do not support appending in which case `append`
   * should never be passed. The default handles this case.
   *
   * @param append Whether to append or overwrite.
   */
  virtual void append_or_set(T new_value, bool append);

public:
  base_setting_t(const T& def, const bool document_default, const std::string& name,
              const std::string& description, const string_set_t& aliases = {},
              std::optional<experimental_feature_t> experimental_feature = std::nullopt)
      : abstract_setting_t(name, description, aliases, experimental_feature),
        value(def),
        default_value(def),
        document_default(document_default) {}

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
  void set_default(const U& v) {
    if (!overridden)
      value = v;
  }

  /**
   * Require any experimental feature the setting depends on
   *
   * Uses `parse()` to get the value from `str`, and `append_or_set()`
   * to set it.
   */
  void set(const std::string& str, bool append = false) override final;

  /**
   * C++ trick; This is template-specialized to compile-time indicate whether
   * the type is appendable.
   */
  struct trait;

  /**
   * always defined based on the C++ magic
   * with `trait` above.
   */
  bool is_appendable() override final;

  virtual void override(const T& v) {
    overridden = true;
    value = v;
  }

  std::string to_string() const override;

  void convert_to_arg(Args& args, const std::string& category) override;

  std::map<std::string, nlohmann::json> to_json_object() const override;
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
  setting_t(config_t* options, const T& def, const std::string& name, const std::string& description,
          const string_set_t& aliases = {}, const bool document_default = true,
          std::optional<experimental_feature_t> experimental_feature = std::nullopt)
      : base_setting_t<T>(def, document_default, name, description, aliases,
                       std::move(experimental_feature)) {
    options->add_setting(this);
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
  path_setting_t(config_t* options, const Path& def, const std::string& name,
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
  optional_path_setting_t(config_t* options, const std::optional<Path>& def, const std::string& name,
                      const std::string& description, const string_set_t& aliases = {});

  std::optional<Path> parse(const std::string& str) const override;

  void operator=(const std::optional<Path>& v);
};

struct experimental_feature_settings_t : config_t {
  setting_t<std::set<experimental_feature_t>> experimental_features{this,
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
  bool is_enabled(const experimental_feature_t&) const;

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
  void require(const experimental_feature_t& feature, GetReason&& get_reason) const {
    if (is_enabled(feature))
      return;
    require(feature, get_reason());
  }

  /**
   * `std::nullopt` pointer means no feature, which means there is nothing that could be
   * disabled, and so the function returns true in that case.
   */
  bool is_enabled(const std::optional<experimental_feature_t>&) const;

  /**
   * `std::nullopt` pointer means no feature, which means there is nothing that could be
   * disabled, and so the function does nothing in that case.
   */
  void require(const std::optional<experimental_feature_t>&) const;
};

// FIXME: don't use a global variable.
extern experimental_feature_settings_t experimental_feature_settings;

} // namespace nix
