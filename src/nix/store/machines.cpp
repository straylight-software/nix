#include "nix/store/machines.h"

#include <algorithm>

#include "nix/store/globals.h"
#include "nix/store/store-open.h"
#include "nix/util/base-n.h"

namespace nix {

Machine::Machine(const std::string& storeUri, decltype(systemTypes) systemTypes,
                 decltype(sshKey) sshKey, decltype(maxJobs) maxJobs,
                 decltype(speedFactor) speedFactor, decltype(supportedFeatures) supportedFeatures,
                 decltype(mandatoryFeatures) mandatoryFeatures,
                 decltype(ssh_public_host_key) ssh_public_host_key)
    : storeUri(StoreReference::parse(
          // Backwards compatibility: if the URI is schemeless, is not a path,
          // and is not one of the special store connection words, prepend
          // ssh://.
          storeUri.find("://") != std::string::npos || storeUri.find("/") != std::string::npos ||
                  storeUri == "auto" || storeUri == "daemon" || storeUri == "local" ||
                  has_prefix(storeUri, "auto?") || has_prefix(storeUri, "daemon?") ||
                  has_prefix(storeUri, "local?") || has_prefix(storeUri, "?")
              ? storeUri
              : "ssh://" + storeUri)),
      systemTypes(systemTypes),
      sshKey(sshKey),
      maxJobs(maxJobs),
      speedFactor(speedFactor == 0.0f ? 1.0f : speedFactor),
      supportedFeatures(supportedFeatures),
      mandatoryFeatures(mandatoryFeatures),
      ssh_public_host_key(ssh_public_host_key) {
  if (speedFactor < 0.0)
    throw UsageError("speed factor must be >= 0");
}

bool Machine::systemSupported(const std::string& system) const {
  return system == "builtin" || (systemTypes.count(system) > 0);
}

bool Machine::allSupported(const string_set_t& features) const {
  return std::all_of(features.begin(), features.end(), [&](const std::string& feature) {
    return supportedFeatures.count(feature) || mandatoryFeatures.count(feature);
  });
}

bool Machine::mandatoryMet(const string_set_t& features) const {
  return std::all_of(mandatoryFeatures.begin(), mandatoryFeatures.end(),
                     [&](const std::string& feature) { return features.count(feature); });
}

StoreReference Machine::completeStoreReference() const {
  auto storeUri = this->storeUri;

  auto* generic = std::get_if<StoreReference::Specified>(&storeUri.variant);

  if (generic && generic->scheme == "ssh") {
    storeUri.params["max-connections"] = "1";
    storeUri.params["log-fd"] = "4";
  }

  if (generic && (generic->scheme == "ssh" || generic->scheme == "ssh-ng")) {
    if (sshKey != "")
      storeUri.params["ssh-key"] = sshKey;
    if (ssh_public_host_key != "")
      storeUri.params["base64-ssh-public-host-key"] = ssh_public_host_key;
  }

  {
    auto& fs = storeUri.params["system-features"];
    auto append = [&](auto feats) {
      for (auto& f : feats) {
        if (fs.size() > 0)
          fs += ' ';
        fs += f;
      }
    };
    append(supportedFeatures);
    append(mandatoryFeatures);
  }

  return storeUri;
}

ref<store_t> Machine::open_store() const {
  return nix::open_store(completeStoreReference());
}

static std::vector<std::string> expand_builder_lines(const std::string& builders) {
  std::vector<std::string> result;
  for (auto line : tokenize_string<std::vector<std::string>>(builders, "\n")) {
    line.erase(std::find(line.begin(), line.end(), '#'), line.end());
    for (auto entry : tokenize_string<std::vector<std::string>>(line, ";")) {
      entry = trim(entry);

      if (entry.empty()) {
        // skip blank entries
      } else if (entry[0] == '@') {
        const std::string path = trim(std::string_view{entry}.substr(1));
        std::string text;
        try {
          text = read_file(path);
        } catch (const sys_error_t& e) {
          if (e.err_no() != ENOENT)
            throw;
          debug("cannot find machines file '%s'", path);
          continue;
        }

        const auto entrys = expand_builder_lines(text);
        result.insert(end(result), begin(entrys), end(entrys));
      } else {
        result.emplace_back(entry);
      }
    }
  }
  return result;
}

static Machine parse_builder_line(const string_set_t& default_systems, const std::string& line) {
  const auto tokens = tokenize_string<std::vector<std::string>>(line);

  auto is_set = [&](size_t field_index) {
    return tokens.size() > field_index && tokens[field_index] != "" && tokens[field_index] != "-";
  };

  auto parse_unsigned_int_field = [&](size_t field_index) {
    const auto result = string2_int<unsigned int>(tokens[field_index]);
    if (!result) {
      throw FormatError("bad machine specification: failed to convert column #%lu in a row: '%s' "
                        "to 'unsigned int'",
                        field_index, line);
    }
    return result.value();
  };

  auto parse_float_field = [&](size_t field_index) {
    const auto result = string2_float<float>(tokens[field_index]);
    if (!result) {
      throw FormatError(
          "bad machine specification: failed to convert column #%lu in a row: '%s' to 'float'",
          field_index, line);
    }
    return result.value();
  };

  auto ensure_base64 = [&](size_t field_index) {
    const auto& str = tokens[field_index];
    try {
      base64::decode(str);
    } catch (FormatError& e) {
      e.add_trace({}, "while parsing machine specification at a column #%lu in a row: '%s'",
                  field_index, line);
      throw;
    }
    return str;
  };

  if (!is_set(0))
    throw FormatError(
        "bad machine specification: store URL was not found at the first column of a row: '%s'",
        line);

  // TODO use designated initializers, once C++ supports those with
  // custom constructors.
  return {// `storeUri`
          tokens[0],
          // `systemTypes`
          is_set(1) ? tokenize_string<string_set_t>(tokens[1], ",") : default_systems,
          // `sshKey`
          is_set(2) ? tokens[2] : "",
          // `maxJobs`
          is_set(3) ? parse_unsigned_int_field(3) : 1U,
          // `speedFactor`
          is_set(4) ? parse_float_field(4) : 1.0f,
          // `supportedFeatures`
          is_set(5) ? tokenize_string<string_set_t>(tokens[5], ",") : string_set_t{},
          // `mandatoryFeatures`
          is_set(6) ? tokenize_string<string_set_t>(tokens[6], ",") : string_set_t{},
          // `sshPublicHostKey`
          is_set(7) ? ensure_base64(7) : ""};
}

static Machines parse_builder_lines(const string_set_t& default_systems,
                                    const std::vector<std::string>& builders) {
  Machines result;
  std::transform(builders.begin(), builders.end(), std::back_inserter(result),
                 [&](auto&& line) { return parse_builder_line(default_systems, line); });
  return result;
}

Machines Machine::parseConfig(const string_set_t& default_systems, const std::string& s) {
  const auto builderLines = expand_builder_lines(s);
  return parse_builder_lines(default_systems, builderLines);
}

Machines get_machines() {
  return Machine::parseConfig({settings.thisSystem}, settings.builders);
}

} // namespace nix
