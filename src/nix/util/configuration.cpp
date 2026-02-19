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

Config::Config(string_map_t initials) : abstract_config_t(std::move(initials)) {}

bool Config::set(const std::string& name, const std::string& value) {
  bool append = false;
  auto i = _settings.find(name);
  if (i == _settings.end()) {
    if (hasPrefix(name, "extra-")) {
      i = _settings.find(std::string(name, 6));
      if (i == _settings.end() || !i->second.setting->isAppendable())
        return false;
      append = true;
    } else
      return false;
  }
  i->second.setting->set(value, append);
  i->second.setting->overridden = true;
  return true;
}

void Config::addSetting(abstract_setting_t* setting) {
  _settings.emplace(setting->name, Config::setting_data_t{false, setting});
  for (const auto& alias : setting->aliases)
    _settings.emplace(alias, Config::setting_data_t{true, setting});

  bool set = false;

  if (auto i = unknownSettings.find(setting->name); i != unknownSettings.end()) {
    setting->set(std::move(i->second));
    setting->overridden = true;
    unknownSettings.erase(i);
    set = true;
  }

  for (auto& alias : setting->aliases) {
    if (auto i = unknownSettings.find(alias); i != unknownSettings.end()) {
      if (set)
        warn("setting '%s' is set, but it's an alias of '%s' which is also set", alias,
             setting->name);
      else {
        setting->set(std::move(i->second));
        setting->overridden = true;
        unknownSettings.erase(i);
        set = true;
      }
    }
  }
}

abstract_config_t::abstract_config_t(string_map_t initials) : unknownSettings(std::move(initials)) {}

void abstract_config_t::warnUnknownSettings() {
  for (const auto& s : unknownSettings)
    warn("unknown setting '%s'", s.first);
}

void abstract_config_t::reapplyUnknownSettings() {
  auto unknownSettings2 = std::move(unknownSettings);
  unknownSettings = {};
  for (auto& s : unknownSettings2)
    set(s.first, s.second);
}

void Config::getSettings(std::map<std::string, setting_info_t>& res, bool overriddenOnly) const {
  for (const auto& opt : _settings)
    if (!opt.second.isAlias && (!overriddenOnly || opt.second.setting->overridden) &&
        experimentalFeatureSettings.isEnabled(opt.second.setting->experimentalFeature))
      res.emplace(opt.first,
                  setting_info_t{opt.second.setting->to_string(), opt.second.setting->description});
}

/**
 * Parse configuration in `contents`, and also the configuration files included from there, with
 * their location specified relative to `path`.
 *
 * `contents` and `path` represent the file that is being parsed.
 * The result is only an intermediate list of key-value pairs of strings.
 * More parsing according to the settings-specific semantics is being done by `loadConfFile` in
 * `libstore/globals.cc`.
 */
static void parseConfigFiles(const std::string& contents, const std::string& path,
                             std::vector<std::pair<std::string, std::string>>& parsedContents) {
  unsigned int pos = 0;

  while (pos < contents.size()) {
    std::string line;
    while (pos < contents.size() && contents[pos] != '\n')
      line += contents[pos++];
    pos++;

    if (auto hash = line.find('#'); hash != line.npos)
      line = std::string(line, 0, hash);

    auto tokens = tokenizeString<std::vector<std::string>>(line);
    if (tokens.empty())
      continue;

    if (tokens.size() < 2)
      throw UsageError("syntax error in configuration line '%1%' in '%2%'", line, path);

    auto include = false;
    auto ignoreMissing = false;
    if (tokens[0] == "include")
      include = true;
    else if (tokens[0] == "!include") {
      include = true;
      ignoreMissing = true;
    }

    if (include) {
      if (tokens.size() != 2)
        throw UsageError("syntax error in configuration line '%1%' in '%2%'", line, path);
      auto p = absPath(tokens[1], dirOf(path));
      if (pathExists(p)) {
        try {
          std::string includedContents = readFile(p);
          parseConfigFiles(includedContents, p, parsedContents);
        } catch (SystemError&) {
          // TODO: Do we actually want to ignore this? Or is it better to fail?
        }
      } else if (!ignoreMissing) {
        throw Error("file '%1%' included from '%2%' not found", p, path);
      }
      continue;
    }

    if (tokens[1] != "=")
      throw UsageError("syntax error in configuration line '%1%' in '%2%'", line, path);

    std::string name = std::move(tokens[0]);

    auto i = tokens.begin();
    advance(i, 2);

    parsedContents.push_back({
        std::move(name),
        concatStringsSep(" ", strings_t(i, tokens.end())),
    });
  };
}

void abstract_config_t::applyConfig(const std::string& contents, const std::string& path) {
  std::vector<std::pair<std::string, std::string>> parsedContents;

  parseConfigFiles(contents, path, parsedContents);

  // First apply experimental-feature related settings
  for (const auto& [name, value] : parsedContents)
    if (name == "experimental-features" || name == "extra-experimental-features")
      set(name, value);

  // Then apply other settings
  // XXX: NIX_PATH must override the regular setting! This is done in `initGC()`
  // Environment variables overriding settings should probably be part of the Config mechanism,
  // but at the time of writing it's not worth building that for just one thing
  for (const auto& [name, value] : parsedContents) {
    if (name != "experimental-features" && name != "extra-experimental-features") {
      if ((name == "nix-path" || name == "extra-nix-path") && getEnv("NIX_PATH").has_value()) {
        continue;
      }
      set(name, value);
    }
  }
}

void Config::resetOverridden() {
  for (auto& s : _settings)
    s.second.setting->overridden = false;
}

nlohmann::json Config::toJSON() {
  auto res = nlohmann::json::object();
  for (const auto& s : _settings)
    if (!s.second.isAlias)
      res.emplace(s.first, s.second.setting->toJSON());
  return res;
}

std::string Config::toKeyValue() {
  std::string res;
  for (const auto& s : _settings)
    if (s.second.isAlias)
      res += fmt("%s = %s\n", s.first, s.second.setting->to_string());
  return res;
}

void Config::convertToArgs(Args& args, const std::string& category) {
  for (auto& s : _settings) {
    if (!s.second.isAlias)
      s.second.setting->convertToArg(args, category);
  }
}

abstract_setting_t::abstract_setting_t(const std::string& name, const std::string& description,
                                 const string_set_t& aliases,
                                 std::optional<experimental_feature_t> experimentalFeature)
    : name(name),
      description(stripIndentation(description)),
      aliases(aliases),
      experimentalFeature(std::move(experimentalFeature)) {}

abstract_setting_t::~abstract_setting_t() {
  // Check against a gcc miscompilation causing our constructor
  // not to run (https://gcc.gnu.org/bugzilla/show_bug.cgi?id=80431).
  assert(created == 123);
}

nlohmann::json abstract_setting_t::toJSON() {
  return nlohmann::json(toJSONObject());
}

std::map<std::string, nlohmann::json> abstract_setting_t::toJSONObject() const {
  std::map<std::string, nlohmann::json> obj;
  obj.emplace("description", description);
  obj.emplace("aliases", aliases);
  if (experimentalFeature)
    obj.emplace("experimentalFeature", *experimentalFeature);
  else
    obj.emplace("experimentalFeature", nullptr);
  return obj;
}

void abstract_setting_t::convertToArg(Args& args, const std::string& category) {}

bool abstract_setting_t::isOverridden() const {
  return overridden;
}

template <>
std::string base_setting_t<std::string>::parse(const std::string& str) const {
  return str;
}

template <>
std::string base_setting_t<std::string>::to_string() const {
  return value;
}

template <>
std::optional<std::string>
base_setting_t<std::optional<std::string>>::parse(const std::string& str) const {
  if (str == "")
    return std::nullopt;
  else
    return {str};
}

template <>
std::string base_setting_t<std::optional<std::string>>::to_string() const {
  return value ? *value : "";
}

template <>
bool base_setting_t<bool>::parse(const std::string& str) const {
  if (str == "true" || str == "yes" || str == "1")
    return true;
  else if (str == "false" || str == "no" || str == "0")
    return false;
  else
    throw UsageError("Boolean setting '%s' has invalid value '%s'", name, str);
}

template <>
std::string base_setting_t<bool>::to_string() const {
  return value ? "true" : "false";
}

template <>
void base_setting_t<bool>::convertToArg(Args& args, const std::string& category) {
  args.addFlag({
      .longName = name,
      .aliases = aliases,
      .description = fmt("Enable the `%s` setting.", name),
      .category = category,
      .handler = {[this] { override(true); }},
      .experimentalFeature = experimentalFeature,
  });
  args.addFlag({
      .longName = "no-" + name,
      .aliases = aliases,
      .description = fmt("Disable the `%s` setting.", name),
      .category = category,
      .handler = {[this] { override(false); }},
      .experimentalFeature = experimentalFeature,
  });
}

template <>
std::list<std::filesystem::path>
base_setting_t<std::list<std::filesystem::path>>::parse(const std::string& str) const {
  auto tokens = tokenizeString<std::list<std::string>>(str);
  return {tokens.begin(), tokens.end()};
}

template <>
strings_t base_setting_t<strings_t>::parse(const std::string& str) const {
  return tokenizeString<strings_t>(str);
}

template <>
void base_setting_t<std::list<std::filesystem::path>>::appendOrSet(
    std::list<std::filesystem::path> newValue, bool append) {
  if (!append)
    value.clear();
  value.insert(value.end(), std::make_move_iterator(newValue.begin()),
               std::make_move_iterator(newValue.end()));
}

template <>
void base_setting_t<strings_t>::appendOrSet(strings_t newValue, bool append) {
  if (!append)
    value.clear();
  value.insert(value.end(), std::make_move_iterator(newValue.begin()),
               std::make_move_iterator(newValue.end()));
}

template <>
std::string base_setting_t<std::list<std::filesystem::path>>::to_string() const {
  return concatStringsSep(" ", value | std::views::transform([](const auto& p) {
                                 return p.string();
                               }) | std::ranges::to<std::list<std::string>>());
}

template <>
std::string base_setting_t<strings_t>::to_string() const {
  return concatStringsSep(" ", value);
}

template <>
string_set_t base_setting_t<string_set_t>::parse(const std::string& str) const {
  return tokenizeString<string_set_t>(str);
}

template <>
void base_setting_t<string_set_t>::appendOrSet(string_set_t newValue, bool append) {
  if (!append)
    value.clear();
  value.insert(std::make_move_iterator(newValue.begin()), std::make_move_iterator(newValue.end()));
}

template <>
std::string base_setting_t<string_set_t>::to_string() const {
  return concatStringsSep(" ", value);
}

template <>
std::set<experimental_feature_t>
base_setting_t<std::set<experimental_feature_t>>::parse(const std::string& str) const {
  std::set<experimental_feature_t> res;
  for (auto& s : tokenizeString<string_set_t>(str)) {
    if (auto thisXpFeature = parseExperimentalFeature(s))
      res.insert(thisXpFeature.value());
    else if (stabilizedFeatures.count(s))
      debug("experimental feature '%s' is now stable", s);
    else
      warn("unknown experimental feature '%s'", s);
  }
  return res;
}

template <>
void base_setting_t<std::set<experimental_feature_t>>::appendOrSet(std::set<experimental_feature_t> newValue,
                                                             bool append) {
  if (!append)
    value.clear();
  value.insert(std::make_move_iterator(newValue.begin()), std::make_move_iterator(newValue.end()));
}

template <>
std::string base_setting_t<std::set<experimental_feature_t>>::to_string() const {
  string_set_t stringifiedXpFeatures;
  for (const auto& feature : value)
    stringifiedXpFeatures.insert(std::string(showExperimentalFeature(feature)));
  return concatStringsSep(" ", stringifiedXpFeatures);
}

template <>
string_map_t base_setting_t<string_map_t>::parse(const std::string& str) const {
  string_map_t res;
  for (const auto& s : tokenizeString<strings_t>(str)) {
    if (auto eq = s.find_first_of('='); s.npos != eq)
      res.emplace(std::string(s, 0, eq), std::string(s, eq + 1));
    // else ignored
  }
  return res;
}

template <>
void base_setting_t<string_map_t>::appendOrSet(string_map_t newValue, bool append) {
  if (!append)
    value.clear();
  value.insert(std::make_move_iterator(newValue.begin()), std::make_move_iterator(newValue.end()));
}

template <>
std::string base_setting_t<string_map_t>::to_string() const {
  return std::transform_reduce(
      value.cbegin(), value.cend(), std::string{},
      [](const auto& l, const auto& r) { return l + " " + r; },
      [](const auto& kvpair) { return kvpair.first + "=" + kvpair.second; });
}

static Path parsePath(const abstract_setting_t& s, const std::string& str) {
  if (str == "")
    throw UsageError("setting '%s' is a path and paths cannot be empty", s.name);
  else
    return canonPath(str);
}

template <>
std::filesystem::path base_setting_t<std::filesystem::path>::parse(const std::string& str) const {
  return parsePath(*this, str);
}

template <>
std::string base_setting_t<std::filesystem::path>::to_string() const {
  return value.string();
}

template <>
std::optional<std::filesystem::path>
base_setting_t<std::optional<std::filesystem::path>>::parse(const std::string& str) const {
  if (str == "")
    return std::nullopt;
  else
    return parsePath(*this, str);
}

template <>
std::string base_setting_t<std::optional<std::filesystem::path>>::to_string() const {
  return value ? value->string() : "";
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

path_setting_t::path_setting_t(Config* options, const Path& def, const std::string& name,
                         const std::string& description, const string_set_t& aliases)
    : base_setting_t<Path>(def, true, name, description, aliases) {
  options->addSetting(this);
}

Path path_setting_t::parse(const std::string& str) const {
  return parsePath(*this, str);
}

optional_path_setting_t::optional_path_setting_t(Config* options, const std::optional<Path>& def,
                                         const std::string& name, const std::string& description,
                                         const string_set_t& aliases)
    : base_setting_t<std::optional<Path>>(def, true, name, description, aliases) {
  options->addSetting(this);
}

std::optional<Path> optional_path_setting_t::parse(const std::string& str) const {
  if (str == "")
    return std::nullopt;
  else
    return parsePath(*this, str);
}

void optional_path_setting_t::operator=(const std::optional<Path>& v) {
  this->assign(v);
}

bool experimental_feature_settings_t::isEnabled(const experimental_feature_t& feature) const {
  // These features are always enabled - they're stable and universally expected.
  // Unlike other experimental features, these cannot be disabled.
  if (feature ==
          xp_t::CaDerivations || // Content-addressed derivations - foundation for reproducible builds
      feature == xp_t::PipeOperators ||     // Pure syntax sugar, no semantic changes
      feature == xp_t::FetchTree ||         // Required by flakes
      feature == xp_t::FetchClosure ||      // Safe, enables better caching
      feature == xp_t::ParseTomlTimestamps) // TOML spec compliance
    return true;
  auto& f = experimentalFeatures.get();
  return std::find(f.begin(), f.end(), feature) != f.end();
}

void experimental_feature_settings_t::require(const experimental_feature_t& feature,
                                          std::string reason) const {
  if (!isEnabled(feature))
    throw missing_experimental_feature_t(feature, std::move(reason));
}

bool experimental_feature_settings_t::isEnabled(
    const std::optional<experimental_feature_t>& feature) const {
  return !feature || isEnabled(*feature);
}

void experimental_feature_settings_t::require(const std::optional<experimental_feature_t>& feature) const {
  if (feature)
    require(*feature);
}

} // namespace nix
