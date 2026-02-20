#include "nix/util/configuration.h"

#include <nlohmann/json.hpp>

#include "nix/util/abstract-setting-to-json.h"
#include "nix/util/args.h"
#include "nix/util/config-impl.h"
#include "nix/util/environment-variables.h"
#include "nix/util/experimental-features.h"
#include "nix/util/file-system.h"
#include "nix/util/strings.h"
#include "nix/util/util.h"

namespace nix {

config_t::config_t(string_map_t initials) : abstract_config_t(std::move(initials)) {}

bool config_t::set(const std::string& name, const std::string& value) {
  bool append = false;
  auto i = settings_.find(name);
  if (i == settings_.end()) {
    if (has_prefix(name, "extra-")) {
      i = settings_.find(std::string(name, 6));
      if (i == settings_.end() || !i->second.setting->is_appendable()) {
        return false;
      }
      append = true;
    } else {
      return false;
    }
  }
  i->second.setting->set(value, append);
  i->second.setting->overridden = true;
  return true;
}

void config_t::add_setting(abstract_setting_t* setting) {
  settings_.emplace(setting->name, config_t::setting_data_t{false, setting});
  for (const auto& alias : setting->aliases) {
    settings_.emplace(alias, config_t::setting_data_t{true, setting});
  }

  bool set = false;

  if (auto i = unknownSettings_.find(setting->name); i != unknownSettings_.end()) {
    setting->set(std::move(i->second));
    setting->overridden = true;
    unknownSettings_.erase(i);
    set = true;
  }

  for (auto& alias : setting->aliases) {
    if (auto i = unknownSettings_.find(alias); i != unknownSettings_.end()) {
      if (set) {
        warn("setting '%s' is set, but it's an alias of '%s' which is also set", alias,
             setting->name);
      } else {
        setting->set(std::move(i->second));
        setting->overridden = true;
        unknownSettings_.erase(i);
        set = true;
      }
    }
  }
}

abstract_config_t::abstract_config_t(string_map_t initials)
    : unknownSettings_(std::move(initials)) {}

void abstract_config_t::warn_unknown_settings() {
  for (const auto& s : unknownSettings_) {
    warn("unknown setting '%s'", s.first);
  }
}

void abstract_config_t::reapply_unknown_settings() {
  auto unknown_settings2 = std::move(unknownSettings_);
  unknownSettings_ = {};
  for (auto& s : unknown_settings2) {
    set(s.first, s.second);
  }
}

void config_t::get_settings(std::map<std::string, setting_info_t>& res,
                            bool overridden_only) const {
  for (const auto& opt : settings_) {
    if (!opt.second.is_alias && (!overridden_only || opt.second.setting->overridden) &&
        experimental_feature_settings.is_enabled(opt.second.setting->experimental_feature)) {
      res.emplace(opt.first,
                  setting_info_t{opt.second.setting->to_string(), opt.second.setting->description});
    }
  }
}

/**
 * Parse configuration in `contents`, and also the configuration files included from there, with
 * their location specified relative to `path`.
 *
 * `contents` and `path` represent the file that is being parsed.
 * The result is only an intermediate list of key-value pairs of strings.
 * More parsing according to the settings-specific semantics is being done by `load_conf_file` in
 * `libstore/globals.cc`.
 */
static void parse_config_files(const std::string& contents, const std::string& path,
                               std::vector<std::pair<std::string, std::string>>& parsed_contents) {
  unsigned int pos = 0;

  while (pos < contents.size()) {
    std::string line;
    while (pos < contents.size() && contents[pos] != '\n') {
      line += contents[pos++];
    }
    pos++;

    if (auto hash = line.find('#'); hash != line.npos) {
      line = std::string(line, 0, hash);
    }

    auto tokens = tokenize_string<std::vector<std::string>>(line);
    if (tokens.empty()) {
      continue;
    }

    if (tokens.size() < 2) {
      throw UsageError("syntax error in configuration line '%1%' in '%2%'", line, path);
    }

    auto include = false;
    auto ignore_missing = false;
    if (tokens[0] == "include") {
      include = true;
    } else if (tokens[0] == "!include") {
      include = true;
      ignore_missing = true;
    }

    if (include) {
      if (tokens.size() != 2) {
        throw UsageError("syntax error in configuration line '%1%' in '%2%'", line, path);
      }
      auto p = abs_path(tokens[1], dir_of(path));
      if (path_exists(p)) {
        try {
          std::string included_contents = read_file(p);
          parse_config_files(included_contents, p, parsed_contents);
        } catch (SystemError&) {
          // TODO: Do we actually want to ignore this? Or is it better to fail?
        }
      } else if (!ignore_missing) {
        throw Error("file '%1%' included from '%2%' not found", p, path);
      }
      continue;
    }

    if (tokens[1] != "=") {
      throw UsageError("syntax error in configuration line '%1%' in '%2%'", line, path);
    }

    std::string name = std::move(tokens[0]);

    auto i = tokens.begin();
    advance(i, 2);

    parsed_contents.push_back({
        std::move(name),
        concat_strings_sep(" ", strings_t(i, tokens.end())),
    });
  };
}

void abstract_config_t::apply_config(const std::string& contents, const std::string& path) {
  std::vector<std::pair<std::string, std::string>> parsed_contents;

  parse_config_files(contents, path, parsed_contents);

  // First apply experimental-feature related settings
  for (const auto& [name, value] : parsed_contents) {
    if (name == "experimental-features" || name == "extra-experimental-features") {
      set(name, value);
    }
  }

  // Then apply other settings
  // XXX: NIX_PATH must override the regular setting! This is done in `initGC()`
  // Environment variables overriding settings should probably be part of the Config mechanism,
  // but at the time of writing it's not worth building that for just one thing
  for (const auto& [name, value] : parsed_contents) {
    if (name != "experimental-features" && name != "extra-experimental-features") {
      if ((name == "nix-path" || name == "extra-nix-path") && get_env("NIX_PATH").has_value()) {
        continue;
      }
      set(name, value);
    }
  }
}

void config_t::reset_overridden() {
  for (auto& s : settings_) {
    s.second.setting->overridden = false;
  }
}

nlohmann::json config_t::to_json() {
  auto res = nlohmann::json::object();
  for (const auto& s : settings_) {
    if (!s.second.is_alias) {
      res.emplace(s.first, s.second.setting->to_json());
    }
  }
  return res;
}

std::string config_t::to_key_value() {
  std::string res;
  for (const auto& s : settings_) {
    if (s.second.is_alias) {
      res += fmt("%s = %s\n", s.first, s.second.setting->to_string());
    }
  }
  return res;
}

void config_t::convert_to_args(Args& args, const std::string& category) {
  for (auto& s : settings_) {
    if (!s.second.is_alias) {
      s.second.setting->convert_to_arg(args, category);
    }
  }
}

abstract_setting_t::abstract_setting_t(const std::string& name, const std::string& description,
                                       const string_set_t& aliases,
                                       std::optional<experimental_feature_t> experimental_feature)
    : name(name),
      description(strip_indentation(description)),
      aliases(aliases),
      experimental_feature(std::move(experimental_feature)) {}

abstract_setting_t::~abstract_setting_t() {
  // Check against a gcc miscompilation causing our constructor
  // not to run (https://gcc.gnu.org/bugzilla/show_bug.cgi?id=80431).
  assert(created == 123);
}

nlohmann::json abstract_setting_t::to_json() {
  return nlohmann::json(to_json_object());
}

std::map<std::string, nlohmann::json> abstract_setting_t::to_json_object() const {
  std::map<std::string, nlohmann::json> obj;
  obj.emplace("description", description);
  obj.emplace("aliases", aliases);
  if (experimental_feature) {
    obj.emplace("experimentalFeature", *experimental_feature);
  } else {
    obj.emplace("experimentalFeature", nullptr);
  }
  return obj;
}

void abstract_setting_t::convert_to_arg(Args& args, const std::string& category) {}

bool abstract_setting_t::is_overridden() const {
  return overridden;
}

template <>
std::string base_setting_t<std::string>::parse(const std::string& str) const {
  return str;
}

template <>
std::string base_setting_t<std::string>::to_string() const {
  return value_;
}

template <>
std::optional<std::string>
base_setting_t<std::optional<std::string>>::parse(const std::string& str) const {
  if (str == "") {
    return std::nullopt;
  } else {
    return {str};
  }
}

template <>
std::string base_setting_t<std::optional<std::string>>::to_string() const {
  return value_ ? *value_ : "";
}

template <>
bool base_setting_t<bool>::parse(const std::string& str) const {
  if (str == "true" || str == "yes" || str == "1") {
    return true;
  } else if (str == "false" || str == "no" || str == "0") {
    return false;
  } else {
    throw UsageError("Boolean setting '%s' has invalid value '%s'", name, str);
  }
}

template <>
std::string base_setting_t<bool>::to_string() const {
  return value_ ? "true" : "false";
}

template <>
void base_setting_t<bool>::convert_to_arg(Args& args, const std::string& category) {
  args.add_flag({
      .long_name = name,
      .aliases = aliases,
      .description = fmt("Enable the `%s` setting.", name),
      .category = category,
      .handler = {[this] { override(true); }},
      .experimental_feature = experimental_feature,
  });
  args.add_flag({
      .long_name = "no-" + name,
      .aliases = aliases,
      .description = fmt("Disable the `%s` setting.", name),
      .category = category,
      .handler = {[this] { override(false); }},
      .experimental_feature = experimental_feature,
  });
}

template <>
std::list<std::filesystem::path>
base_setting_t<std::list<std::filesystem::path>>::parse(const std::string& str) const {
  auto tokens = tokenize_string<std::list<std::string>>(str);
  return {tokens.begin(), tokens.end()};
}

template <>
strings_t base_setting_t<strings_t>::parse(const std::string& str) const {
  return tokenize_string<strings_t>(str);
}

template <>
void base_setting_t<std::list<std::filesystem::path>>::append_or_set(
    std::list<std::filesystem::path> new_value, bool append) {
  if (!append) {
    value_.clear();
  }
  value_.insert(value_.end(), std::make_move_iterator(new_value.begin()),
                std::make_move_iterator(new_value.end()));
}

template <>
void base_setting_t<strings_t>::append_or_set(strings_t new_value, bool append) {
  if (!append) {
    value_.clear();
  }
  value_.insert(value_.end(), std::make_move_iterator(new_value.begin()),
                std::make_move_iterator(new_value.end()));
}

template <>
std::string base_setting_t<std::list<std::filesystem::path>>::to_string() const {
  return concat_strings_sep(" ", value_ | std::views::transform([](const auto& p) {
                                   return p.string();
                                 }) | std::ranges::to<std::list<std::string>>());
}

template <>
std::string base_setting_t<strings_t>::to_string() const {
  return concat_strings_sep(" ", value_);
}

template <>
string_set_t base_setting_t<string_set_t>::parse(const std::string& str) const {
  return tokenize_string<string_set_t>(str);
}

template <>
void base_setting_t<string_set_t>::append_or_set(string_set_t new_value, bool append) {
  if (!append) {
    value_.clear();
  }
  value_.insert(std::make_move_iterator(new_value.begin()),
                std::make_move_iterator(new_value.end()));
}

template <>
std::string base_setting_t<string_set_t>::to_string() const {
  return concat_strings_sep(" ", value_);
}

template <>
std::set<experimental_feature_t>
base_setting_t<std::set<experimental_feature_t>>::parse(const std::string& str) const {
  std::set<experimental_feature_t> res;
  for (auto& s : tokenize_string<string_set_t>(str)) {
    if (auto this_xp_feature = parse_experimental_feature(s)) {
      res.insert(this_xp_feature.value());
    } else if (stabilized_features.count(s)) {
      debug("experimental feature '%s' is now stable", s);
    } else {
      warn("unknown experimental feature '%s'", s);
    }
  }
  return res;
}

template <>
void base_setting_t<std::set<experimental_feature_t>>::append_or_set(
    std::set<experimental_feature_t> new_value, bool append) {
  if (!append) {
    value_.clear();
  }
  value_.insert(std::make_move_iterator(new_value.begin()),
                std::make_move_iterator(new_value.end()));
}

template <>
std::string base_setting_t<std::set<experimental_feature_t>>::to_string() const {
  string_set_t stringified_xp_features;
  for (const auto& feature : value_) {
    stringified_xp_features.insert(std::string(show_experimental_feature(feature)));
  }
  return concat_strings_sep(" ", stringified_xp_features);
}

template <>
string_map_t base_setting_t<string_map_t>::parse(const std::string& str) const {
  string_map_t res;
  for (const auto& s : tokenize_string<strings_t>(str)) {
    if (auto eq = s.find_first_of('='); s.npos != eq) {
      res.emplace(std::string(s, 0, eq), std::string(s, eq + 1));
    }
    // else ignored
  }
  return res;
}

template <>
void base_setting_t<string_map_t>::append_or_set(string_map_t new_value, bool append) {
  if (!append) {
    value_.clear();
  }
  value_.insert(std::make_move_iterator(new_value.begin()),
                std::make_move_iterator(new_value.end()));
}

template <>
std::string base_setting_t<string_map_t>::to_string() const {
  return std::transform_reduce(
      value_.cbegin(), value_.cend(), std::string{},
      [](const auto& l, const auto& r) { return l + " " + r; },
      [](const auto& kvpair) { return kvpair.first + "=" + kvpair.second; });
}

static Path parse_path(const abstract_setting_t& s, const std::string& str) {
  if (str == "") {
    throw UsageError("setting '%s' is a path and paths cannot be empty", s.name);
  } else {
    return canon_path(str);
  }
}

template <>
std::filesystem::path base_setting_t<std::filesystem::path>::parse(const std::string& str) const {
  return parse_path(*this, str);
}

template <>
std::string base_setting_t<std::filesystem::path>::to_string() const {
  return value_.string();
}

template <>
std::optional<std::filesystem::path>
base_setting_t<std::optional<std::filesystem::path>>::parse(const std::string& str) const {
  if (str == "") {
    return std::nullopt;
  } else {
    return parse_path(*this, str);
  }
}

template <>
std::string base_setting_t<std::optional<std::filesystem::path>>::to_string() const {
  return value_ ? value_->string() : "";
}

template class base_setting_t<int>;
template class base_setting_t<unsigned int>;
template class base_setting_t<long>;
template class base_setting_t<unsigned long>;
template class base_setting_t<long long>;
template class base_setting_t<unsigned long long>;
template class base_setting_t<bool>;
template class base_setting_t<std::string>;
template class base_setting_t<std::list<std::filesystem::path>>;
template class base_setting_t<strings_t>;
template class base_setting_t<string_set_t>;
template class base_setting_t<string_map_t>;
template class base_setting_t<std::set<experimental_feature_t>>;
template class base_setting_t<std::filesystem::path>;
template class base_setting_t<std::optional<std::filesystem::path>>;

path_setting_t::path_setting_t(config_t* options, const Path& def, const std::string& name,
                               const std::string& description, const string_set_t& aliases)
    : base_setting_t<Path>(def, true, name, description, aliases) {
  options->add_setting(this);
}

Path path_setting_t::parse(const std::string& str) const {
  return parse_path(*this, str);
}

optional_path_setting_t::optional_path_setting_t(config_t* options, const std::optional<Path>& def,
                                                 const std::string& name,
                                                 const std::string& description,
                                                 const string_set_t& aliases)
    : base_setting_t<std::optional<Path>>(def, true, name, description, aliases) {
  options->add_setting(this);
}

std::optional<Path> optional_path_setting_t::parse(const std::string& str) const {
  if (str == "") {
    return std::nullopt;
  } else {
    return parse_path(*this, str);
  }
}

void optional_path_setting_t::operator=(const std::optional<Path>& v) {
  this->assign(v);
}

bool experimental_feature_settings_t::is_enabled(const experimental_feature_t& feature) const {
  // These features are always enabled - they're stable and universally expected.
  // Unlike other experimental features, these cannot be disabled.
  if (feature == xp_t::ca_derivations ||        // Content-addressed derivations - foundation for
                                                // reproducible builds
      feature == xp_t::pipe_operators ||        // Pure syntax sugar, no semantic changes
      feature == xp_t::fetch_tree ||            // Required by flakes
      feature == xp_t::fetch_closure ||         // Safe, enables better caching
      feature == xp_t::parse_toml_timestamps) { // TOML spec compliance
    return true;
  }
  auto& f = experimental_features.get();
  return std::find(f.begin(), f.end(), feature) != f.end();
}

void experimental_feature_settings_t::require(const experimental_feature_t& feature,
                                              std::string reason) const {
  if (!is_enabled(feature)) {
    throw missing_experimental_feature_t(feature, std::move(reason));
  }
}

bool experimental_feature_settings_t::is_enabled(
    const std::optional<experimental_feature_t>& feature) const {
  return !feature || is_enabled(*feature);
}

void experimental_feature_settings_t::require(
    const std::optional<experimental_feature_t>& feature) const {
  if (feature) {
    require(*feature);
  }
}

} // namespace nix
