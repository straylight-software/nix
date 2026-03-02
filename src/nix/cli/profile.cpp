#include <iomanip>
#include <regex>

#include <nlohmann/json.hpp>

#include "nix-env/user-env.h"
#include "nix/cmd/command.h"
#include "nix/cmd/installable-flake.h"
#include "nix/flake/flakeref.h"
#include "nix/flake/url-name.h"
#include "nix/main/common-args.h"
#include "nix/main/shared.h"
#include "nix/store/builtins/buildenv.h"
#include "nix/store/derivations.h"
#include "nix/store/names.h"
#include "nix/store/profiles.h"
#include "nix/store/store-api.h"
#include "nix/util/archive.h"
#include "nix/util/strings.h"
#include "nix/util/url.h"

namespace nix {

struct profile_element_source_t {
  flake_ref_t original_ref;
  // FIXME: record original attrpath.
  flake_ref_t locked_ref;
  std::string attr_path;
  ExtendedOutputsSpec outputs;

  // TODO libc++ 16 (used by darwin) missing `std::set::operator <=>`, can't do yet.
  // auto operator <=> (const ProfileElementSource & other) const
  auto operator<(const profile_element_source_t& other) const {
    return std::tuple(original_ref.to_string(), attr_path, outputs) <
           std::tuple(other.original_ref.to_string(), other.attr_path, other.outputs);
  }

  std::string to_string() const {
    return fmt("%s#%s%s", original_ref, attr_path, outputs.to_string());
  }
};

const int default_priority = 5;

struct profile_element_t {
  store_path_set_t store_paths;
  std::optional<profile_element_source_t> source;
  bool active = true;
  int priority = default_priority;

  std::string identifier() const {
    if (source) {
      return source->to_string();
    }
    string_set_t names;
    for (auto& path : store_paths) {
      names.insert(DrvName(path.name()).name);
    }
    return drop_empty_init_then_concat_strings_sep(", ", names);
  }

  /**
   * Return a string representing an installable corresponding to the current
   * element, either a flakeref or a plain store path
   */
  string_set_t to_installables(store_t& store) {
    if (source) {
      return {source->to_string()};
    }
    string_set_t raw_paths;
    for (auto& path : store_paths) {
      raw_paths.insert(store.printStorePath(path));
    }
    return raw_paths;
  }

  std::string versions() const {
    string_set_t versions;
    for (auto& path : store_paths) {
      versions.insert(DrvName(path.name()).version);
    }
    return show_versions(versions);
  }

  void update_store_paths(ref<store_t> eval_store, ref<store_t> store,
                          const BuiltPaths& built_paths) {
    store_paths.clear();
    for (auto& buildable : built_paths) {
      std::visit(overloaded{
                     [&](const BuiltPath::opaque_t& bo) { store_paths.insert(bo.path); },
                     [&](const BuiltPath::Built& bfd) {
                       for (auto& output : bfd.outputs) {
                         store_paths.insert(output.second);
                       }
                     },
                 },
                 buildable.raw());
    }
  }
};

std::string get_name_from_element(const profile_element_t& element) {
  std::optional<std::string> result = std::nullopt;
  if (element.source) {
    // Seems to be for flake_t URLs
    result = get_name_from_url(parse_url(element.source->to_string(), /*lenient=*/true));
  }
  return result.value_or(element.identifier());
}

struct profile_manifest_t {
  using ProfileElementName = std::string;

  std::map<ProfileElementName, profile_element_t> elements;

  profile_manifest_t() {}

  profile_manifest_t(eval_state_t& state, const std::filesystem::path& profile) {
    auto manifest_path = profile / "manifest.json";

    if (std::filesystem::exists(manifest_path)) {
      auto json = nlohmann::json::parse(read_file(manifest_path.string()));

      auto version = json.value("version", 0);
      std::string s_url;
      std::string s_original_url;
      switch (version) {
        case 1:
          s_url = "uri";
          s_original_url = "originalUri";
          break;
        case 2:
        case 3:
          s_url = "url";
          s_original_url = "originalUrl";
          break;
        default:
          throw Error("profile manifest '%s' has unsupported version %d", manifest_path, version);
      }

      auto elems = json["elements"];
      for (auto& elem : elems.items()) {
        auto& e = elem.value();
        profile_element_t element;
        for (auto& p : e["storePaths"]) {
          element.store_paths.insert(state.store->parseStorePath((std::string)p));
        }
        element.active = e["active"];
        if (e.contains("priority")) {
          element.priority = e["priority"];
        }
        if (e.value(s_url, "") != "") {
          element.source =
              profile_element_source_t{parse_flake_ref(fetch_settings, e[s_original_url]),
                                       parse_flake_ref(fetch_settings, e[s_url]), e["attrPath"],
                                       e["outputs"].get<ExtendedOutputsSpec>()};
        }

        std::string name = [&] {
          if (elems.is_object()) {
            return elem.key();
          }
          if (element.source) {
            if (auto optName =
                    get_name_from_url(parse_url(element.source->to_string(), /*lenient=*/true))) {
              return *optName;
            }
          }
          return element.identifier();
        }();

        add_element(name, std::move(element));
      }
    }

    else if (std::filesystem::exists(profile / "manifest.nix")) {
      // FIXME: needed because of pure mode; ugly.
      state.allowPath(state.store->followLinksToStorePath(profile.string()));
      state.allowPath(state.store->followLinksToStorePath((profile / "manifest.nix").string()));

      auto package_infos =
          query_installed(state, state.store->followLinksToStore(profile.string()));

      for (auto& package_info : package_infos) {
        profile_element_t element;
        element.store_paths = {package_info.queryOutPath()};
        add_element(std::move(element));
      }
    }
  }

  void add_element(std::string_view name_candidate, profile_element_t element) {
    std::string finalName(name_candidate);
    for (int i = 1; elements.contains(finalName); ++i) {
      finalName = name_candidate + "-" + std::to_string(i);
    }

    elements.insert_or_assign(finalName, std::move(element));
  }

  void add_element(profile_element_t element) {
    auto name = get_name_from_element(element);
    add_element(name, std::move(element));
  }

  nlohmann::json to_json(store_t& store) const {
    auto es = nlohmann::json::object();
    for (auto& [name, element] : elements) {
      auto paths = nlohmann::json::array();
      for (auto& path : element.store_paths) {
        paths.push_back(store.printStorePath(path));
      }
      nlohmann::json obj;
      obj["storePaths"] = paths;
      obj["active"] = element.active;
      obj["priority"] = element.priority;
      if (element.source) {
        obj["originalUrl"] = element.source->original_ref.to_string();
        obj["url"] = element.source->locked_ref.to_string();
        obj["attrPath"] = element.source->attr_path;
        obj["outputs"] = element.source->outputs;
      }
      es[name] = obj;
    }
    nlohmann::json json;
    // Only upgrade with great care as changing it can break fresh installs
    // like in https://github.com/NixOS/nix/issues/10109
    json["version"] = 3;
    json["elements"] = es;
    return json;
  }

  store_path_t build(ref<store_t> store) {
    auto temp_dir = create_temp_dir();

    store_path_set_t references;

    Packages pkgs;
    for (auto& [name, element] : elements) {
      for (auto& path : element.store_paths) {
        if (element.active) {
          pkgs.emplace_back(store->printStorePath(path), true, element.priority);
        }
        references.insert(path);
      }
    }

    build_profile(temp_dir.string(), std::move(pkgs));

    write_file(temp_dir / "manifest.json", to_json(*store).dump());

    /* Add the symlink tree to the store. */
    string_sink_t sink;
    dump_path(temp_dir.string(), sink);

    auto nar_hash = hash_string(hash_algorithm_t::SHA256, sink.str());

    auto info = valid_path_info_t::makeFromCA(*store, "profile",
                                              FixedOutputInfo{
                                                  .method = file_ingestion_method_t::nix_archive,
                                                  .hash = nar_hash,
                                                  .references =
                                                      {
                                                          .others = std::move(references),
                                                          // profiles never refer to themselves
                                                          .self = false,
                                                      },
                                              },
                                              nar_hash);
    info.nar_size = sink.str().size();

    string_source_t source(sink.str());
    store->add_to_store(info, source);

    return std::move(info.path);
  }

  static void print_diff(const profile_manifest_t& prev, const profile_manifest_t& cur,
                         std::string_view indent) {
    auto i = prev.elements.begin();
    auto j = cur.elements.begin();

    bool changes = false;

    while (i != prev.elements.end() || j != cur.elements.end()) {
      if (j != cur.elements.end() && (i == prev.elements.end() || i->first > j->first)) {
        logger->cout("%s%s: %s added", indent, j->second.identifier(), j->second.versions());
        changes = true;
        ++j;
      } else if (i != prev.elements.end() && (j == cur.elements.end() || i->first < j->first)) {
        logger->cout("%s%s: %s removed", indent, i->second.identifier(), i->second.versions());
        changes = true;
        ++i;
      } else {
        auto v1 = i->second.versions();
        auto v2 = j->second.versions();
        if (v1 != v2) {
          logger->cout("%s%s: %s -> %s", indent, i->second.identifier(), v1, v2);
          changes = true;
        }
        ++i;
        ++j;
      }
    }

    if (!changes) {
      logger->cout("%sNo changes.", indent);
    }
  }
};

static std::map<Installable*, std::pair<BuiltPaths, ref<ExtraPathInfo>>>
builtPathsPerInstallable(const std::vector<InstallableWithBuildResult>& built_paths) {
  std::map<Installable*, std::pair<BuiltPaths, ref<ExtraPathInfo>>> res;
  for (auto& b : built_paths) {
    auto& r = res.insert({&*b.installable,
                          {
                              {},
                              make_ref<ExtraPathInfo>(),
                          }})
                  .first->second;
    /* Note that there could be conflicting info
       (e.g. meta.priority fields) if the installable returned
       multiple derivations. So pick one arbitrarily. FIXME:
       print a warning? */
    auto builtPath = b.getSuccess();
    r.first.push_back(builtPath.path);
    r.second = builtPath.info;
  }
  return res;
}

struct cmd_profile_add_t : InstallablesCommand, MixDefaultProfile {
  std::optional<int64_t> priority;

  cmd_profile_add_t() {
    add_flag({
        .long_name = "priority",
        .description = "The priority of the package to add.",
        .labels = {"priority"},
        .handler = {&priority},
    });
  };

  std::string description() override { return "add a package to a profile"; }

  std::string doc() override {
    return
#include "profile-add.md"
        ;
  }

  void run(ref<store_t> store, Installables&& installables) override {
    profile_manifest_t manifest(*getEvalState(), *profile);

    auto build_results =
        Installable::build2(getEvalStore(), store, Realise::Outputs, installables, bmNormal);
    Installable::throwBuildErrors(build_results, *store);

    auto built_paths = builtPathsPerInstallable(build_results);

    for (auto& installable : installables) {
      profile_element_t element;

      auto iter = built_paths.find(&*installable);
      if (iter == built_paths.end()) {
        continue;
      }
      auto& [res, info] = iter->second;

      if (auto* info2 = dynamic_cast<ExtraPathInfoFlake*>(&*info)) {
        element.source = profile_element_source_t{
            .original_ref = info2->flake.original_ref,
            .locked_ref = info2->flake.locked_ref,
            .attr_path = info2->value.attr_path,
            .outputs = info2->value.extendedOutputsSpec,
        };
      }

      // If --priority was specified we want to override the
      // priority of the installable.
      element.priority = priority ? *priority : ({
        auto* info2 = dynamic_cast<ExtraPathInfoValue*>(&*info);
        info2 ? info2->value.priority.value_or(default_priority) : default_priority;
      });

      element.update_store_paths(getEvalStore(), store, res);

      auto elementName = get_name_from_element(element);

      // Check if the element already exists.
      auto existingPair = manifest.elements.find(elementName);
      if (existingPair != manifest.elements.end()) {
        auto existingElement = existingPair->second;
        auto existingSource = existingElement.source;
        auto elementSource = element.source;
        if (existingSource && elementSource && existingElement.priority == element.priority &&
            existingSource->original_ref == elementSource->original_ref &&
            existingSource->attr_path == elementSource->attr_path) {
          warn("'%s' is already added", elementName);
          continue;
        }
      }

      manifest.add_element(elementName, std::move(element));
    }

    try {
      updateProfile(manifest.build(store));
    } catch (BuildEnvFileConflictError& conflict_error) {
      // FIXME use C++20 std::ranges once macOS has it
      //       See
      //       https://github.com/NixOS/nix/compare/3efa476c5439f8f6c1968a6ba20a31d1239c2f04..1fe5d172ece51a619e879c4b86f603d9495cc102
      auto find_ref_by_file_path = [&]<typename Iterator>(Iterator begin, Iterator end) {
        for (auto it = begin; it != end; it++) {
          auto& [name, profileElement] = *it;
          for (auto& store_path : profileElement.store_paths) {
            if (conflict_error.fileA.starts_with(store->printStorePath(store_path))) {
              return std::tuple(conflict_error.fileA, name, profileElement.to_installables(*store));
            }
            if (conflict_error.fileB.starts_with(store->printStorePath(store_path))) {
              return std::tuple(conflict_error.fileB, name, profileElement.to_installables(*store));
            }
          }
        }
        throw conflict_error;
      };
      // There are 2 conflicting files. We need to find out which one is from the already installed
      // package and which one is the package that is the new package that is being installed. The
      // first matching package is the one that was already installed (original).
      auto [originalConflictingFilePath, originalEntryName, originalConflictingRefs] =
          find_ref_by_file_path(manifest.elements.begin(), manifest.elements.end());
      // The last matching package is the one that was going to be installed (new).
      auto [newConflictingFilePath, newEntryName, newConflictingRefs] =
          find_ref_by_file_path(manifest.elements.rbegin(), manifest.elements.rend());

      throw Error("An existing package already provides the following file:\n"
                  "\n"
                  "  %1%\n"
                  "\n"
                  "This is the conflicting file from the new package:\n"
                  "\n"
                  "  %2%\n"
                  "\n"
                  "To remove the existing package:\n"
                  "\n"
                  "  nix profile remove %3%\n"
                  "\n"
                  "The new package can also be added next to the existing one by assigning a "
                  "different priority.\n"
                  "The conflicting packages have a priority of %5%.\n"
                  "To prioritise the new package:\n"
                  "\n"
                  "  nix profile add %4% --priority %6%\n"
                  "\n"
                  "To prioritise the existing package:\n"
                  "\n"
                  "  nix profile add %4% --priority %7%\n",
                  originalConflictingFilePath, newConflictingFilePath, originalEntryName,
                  concat_strings_sep(" ", newConflictingRefs), conflict_error.priority,
                  conflict_error.priority - 1, conflict_error.priority + 1);
    }
  }
};

struct matcher_t {
  virtual ~matcher_t() {}

  virtual std::string get_title() = 0;
  virtual bool matches(const std::string& name, const profile_element_t& element) = 0;
};

struct regex_matcher_t final : public matcher_t {
  std::regex regex;
  std::string pattern;

  regex_matcher_t(const std::string& pattern)
      : regex(pattern, std::regex::extended | std::regex::icase), pattern(pattern) {}

  std::string get_title() override { return fmt("Regex '%s'", pattern); }

  bool matches(const std::string& name, const profile_element_t& element) override {
    return std::regex_match(element.identifier(), regex);
  }
};

struct store_path_matcher_t final : public matcher_t {
  store_path_t store_path;

  store_path_matcher_t(const store_path_t& store_path) : store_path(store_path) {}

  std::string get_title() override { return fmt("store_t path '%s'", store_path.to_string()); }

  bool matches(const std::string& name, const profile_element_t& element) override {
    return element.store_paths.count(store_path);
  }
};

struct name_matcher_t final : public matcher_t {
  std::string name;

  name_matcher_t(const std::string& name) : name(name) {}

  std::string get_title() override { return fmt("Package name '%s'", name); }

  bool matches(const std::string& name, const profile_element_t& element) override {
    return name == this->name;
  }
};

struct all_matcher_t final : public matcher_t {
  std::string get_title() override { return "--all"; }

  bool matches(const std::string& name, const profile_element_t& element) override { return true; }
};

all_matcher_t all;

struct mix_profile_element_matchers_t : virtual args_t, virtual StoreCommand {
  std::vector<ref<matcher_t>> _matchers;

  mix_profile_element_matchers_t() {
    add_flag({
        .long_name = "all",
        .description = "Match all packages in the profile.",
        .handler = {[this]() {
          _matchers.push_back(
              ref<all_matcher_t>(std::shared_ptr<all_matcher_t>(&all, [](all_matcher_t*) {})));
        }},
    });
    add_flag({
        .long_name = "regex",
        .description = "A regular expression to match one or more packages in the profile.",
        .labels = {"pattern"},
        .handler = {[this](std::string arg) {
          _matchers.push_back(make_ref<regex_matcher_t>(arg));
        }},
    });
    expect_args({.label = "elements",
                 .optional = true,
                 .handler = {[this](std::vector<std::string> args) {
                   for (auto& arg : args) {
                     if (auto n = string2_int<size_t>(arg)) {
                       throw Error("'nix profile' no longer supports indices ('%d')", *n);
                     } else if (getStore()->isStorePath(arg)) {
                       _matchers.push_back(
                           make_ref<store_path_matcher_t>(getStore()->parseStorePath(arg)));
                     } else {
                       _matchers.push_back(make_ref<name_matcher_t>(arg));
                     }
                   }
                 }}});
  }

  string_set_t get_matching_element_names(profile_manifest_t& manifest) {
    if (_matchers.empty()) {
      throw UsageError("No packages specified.");
    }

    if (std::find_if(_matchers.begin(), _matchers.end(),
                     [](const ref<matcher_t>& m) {
                       return m.dynamic_pointer_cast<all_matcher_t>();
                     }) != _matchers.end() &&
        _matchers.size() > 1) {
      throw UsageError("--all cannot be used with package names or regular expressions.");
    }

    if (manifest.elements.empty()) {
      warn("There are no packages in the profile.");
      return {};
    }

    string_set_t result;
    for (auto& matcher : _matchers) {
      bool foundMatch = false;
      for (auto& [name, element] : manifest.elements) {
        if (matcher->matches(name, element)) {
          result.insert(name);
          foundMatch = true;
        }
      }
      if (!foundMatch) {
        warn("%s does not match any packages in the profile.", matcher->get_title());
      }
    }
    return result;
  }
};

struct cmd_profile_remove_t : virtual EvalCommand,
                              MixDefaultProfile,
                              mix_profile_element_matchers_t {
  std::string description() override { return "remove packages from a profile"; }

  std::string doc() override {
    return
#include "profile-remove.md"
        ;
  }

  void run(ref<store_t> store) override {
    profile_manifest_t old_manifest(*getEvalState(), *profile);

    profile_manifest_t new_manifest = old_manifest;

    auto matching_element_names = get_matching_element_names(old_manifest);

    if (matching_element_names.empty()) {
      warn("No packages to remove. Use 'nix profile list' to see the current profile.");
      return;
    }

    for (auto& name : matching_element_names) {
      auto& element = old_manifest.elements[name];
      notice("removing '%s'", element.identifier());
      new_manifest.elements.erase(name);
    }

    auto removed_count = old_manifest.elements.size() - new_manifest.elements.size();
    printInfo("removed %d packages, kept %d packages", removed_count, new_manifest.elements.size());

    updateProfile(new_manifest.build(store));
  }
};

struct cmd_profile_upgrade_t : virtual SourceExprCommand,
                               MixDefaultProfile,
                               mix_profile_element_matchers_t {
  std::string description() override { return "upgrade packages using their most recent flake"; }

  std::string doc() override {
    return
#include "profile-upgrade.md"
        ;
  }

  void run(ref<store_t> store) override {
    profile_manifest_t manifest(*getEvalState(), *profile);

    Installables installables;
    std::vector<profile_element_t*> elems;

    auto upgraded_count = 0;

    auto matching_element_names = get_matching_element_names(manifest);

    if (matching_element_names.empty()) {
      warn("No packages to upgrade. Use 'nix profile list' to see the current profile.");
      return;
    }

    for (auto& name : matching_element_names) {
      auto& element = manifest.elements[name];

      if (!element.source) {
        warn("Found package '%s', but it was not added from a flake, so it can't be checked for "
             "upgrades!",
             element.identifier());
        continue;
      }
      if (element.source->original_ref.input.isLocked(getEvalState()->fetch_settings)) {
        warn("Found package '%s', but it was added from a locked flake reference so it can't be "
             "upgraded!",
             element.identifier());
        continue;
      }

      upgraded_count++;

      activity_t act(*logger, lvl_chatty, act_unknown,
                     fmt("checking '%s' for updates", element.source->attr_path));

      auto installable = make_ref<InstallableFlake>(
          this, getEvalState(), flake_ref_t(element.source->original_ref), "",
          element.source->outputs, strings_t{element.source->attr_path}, strings_t{}, lock_flags);

      auto derivedPaths = installable->to_derived_paths();
      if (derivedPaths.empty()) {
        continue;
      }
      auto* infop = dynamic_cast<ExtraPathInfoFlake*>(&*derivedPaths[0].info);
      // `InstallableFlake` should use `ExtraPathInfoFlake`.
      assert(infop);
      auto& info = *infop;

      if (info.flake.locked_ref.input.isLocked(getEvalState()->fetch_settings) &&
          element.source->locked_ref == info.flake.locked_ref) {
        continue;
      }

      printInfo("upgrading '%s' from flake '%s' to '%s'", element.source->attr_path,
                element.source->locked_ref, info.flake.locked_ref);

      element.source = profile_element_source_t{
          .original_ref = installable->flake_ref,
          .locked_ref = info.flake.locked_ref,
          .attr_path = info.value.attr_path,
          .outputs = installable->extendedOutputsSpec,
      };

      installables.push_back(installable);
      elems.push_back(&element);
    }

    if (upgraded_count == 0) {
      warn("Found some packages but none of them could be upgraded.");
      return;
    }

    auto build_results =
        Installable::build2(getEvalStore(), store, Realise::Outputs, installables, bmNormal);
    Installable::throwBuildErrors(build_results, *store);

    auto built_paths = builtPathsPerInstallable(build_results);

    for (size_t i = 0; i < installables.size(); ++i) {
      auto& installable = installables.at(i);
      auto& element = *elems.at(i);
      element.update_store_paths(getEvalStore(), store,
                                 built_paths.find(&*installable)->second.first);
    }

    updateProfile(manifest.build(store));
  }
};

struct cmd_profile_list_t : virtual EvalCommand, virtual StoreCommand, MixDefaultProfile, MixJSON {
  std::string description() override { return "list packages in the profile"; }

  std::string doc() override {
    return
#include "profile-list.md"
        ;
  }

  void run(ref<store_t> store) override {
    profile_manifest_t manifest(*getEvalState(), *profile);

    if (json) {
      printJSON(manifest.to_json(*store));
    } else {
      for (const auto& [i, e] : enumerate(manifest.elements)) {
        auto& [name, element] = e;
        if (i) {
          logger->cout("");
        }
        logger->cout("Name:               " ANSI_BOLD "%s" ANSI_NORMAL "%s", name,
                     element.active ? "" : " " ANSI_RED "(inactive)" ANSI_NORMAL);
        if (element.source) {
          logger->cout("flake_t attribute:    %s%s", element.source->attr_path,
                       element.source->outputs.to_string());
          logger->cout("Original flake URL: %s", element.source->original_ref.to_string());
          logger->cout("Locked flake URL:   %s", element.source->locked_ref.to_string());
        }
        logger->cout("store_t paths:        %s",
                     concat_strings_sep(" ", store->printStorePathSet(element.store_paths)));
      }
    }
  }
};

struct cmd_profile_diff_closures_t : virtual StoreCommand, MixDefaultProfile {
  std::string description() override {
    return "show the closure difference between each version of a profile";
  }

  std::string doc() override {
    return
#include "profile-diff-closures.md"
        ;
  }

  void run(ref<store_t> store) override {
    auto [gens, cur_gen] = findGenerations(*profile);

    std::optional<Generation> prevGen;
    bool first = true;

    for (auto& gen : gens) {
      if (prevGen) {
        if (!first) {
          logger->cout("");
        }
        first = false;
        logger->cout("Version %d -> %d:", prevGen->number, gen.number);
        print_closure_diff(store, store->followLinksToStorePath(prevGen->path.string()),
                           store->followLinksToStorePath(gen.path.string()), "  ");
      }

      prevGen = gen;
    }
  }
};

struct cmd_profile_history_t : virtual StoreCommand, EvalCommand, MixDefaultProfile {
  std::string description() override { return "show all versions of a profile"; }

  std::string doc() override {
    return
#include "profile-history.md"
        ;
  }

  void run(ref<store_t> store) override {
    auto [gens, cur_gen] = findGenerations(*profile);

    std::optional<std::pair<Generation, profile_manifest_t>> prevGen;
    bool first = true;

    for (auto& gen : gens) {
      profile_manifest_t manifest(*getEvalState(), gen.path);

      if (!first) {
        logger->cout("");
      }
      first = false;

      logger->cout(
          "Version %s%d" ANSI_NORMAL " (%s)%s:", gen.number == cur_gen ? ANSI_GREEN : ANSI_BOLD,
          gen.number, std::put_time(std::gmtime(&gen.creationTime), "%Y-%m-%d"),
          prevGen ? fmt(" <- %d", prevGen->first.number) : "");

      profile_manifest_t::print_diff(prevGen ? prevGen->second : profile_manifest_t(), manifest,
                                     "  ");

      prevGen = {gen, std::move(manifest)};
    }
  }
};

struct cmd_profile_rollback_t : virtual StoreCommand, MixDefaultProfile, MixDryRun {
  std::optional<GenerationNumber> version;

  cmd_profile_rollback_t() {
    add_flag({
        .long_name = "to",
        .description = "The profile version to roll back to.",
        .labels = {"version"},
        .handler = {&version},
    });
  }

  std::string description() override {
    return "roll back to the previous version or a specified version of a profile";
  }

  std::string doc() override {
    return
#include "profile-rollback.md"
        ;
  }

  void run(ref<store_t> store) override { switch_generation(*profile, version, dry_run); }
};

struct cmd_profile_wipe_history_t : virtual StoreCommand, MixDefaultProfile, MixDryRun {
  std::optional<std::string> min_age;

  cmd_profile_wipe_history_t() {
    add_flag({
        .long_name = "older-than",
        .description = "Delete versions older than the specified age. *age* "
                       "must be in the format *N*`d`, where *N* denotes a number "
                       "of days.",
        .labels = {"age"},
        .handler = {&min_age},
    });
  }

  std::string description() override { return "delete non-current versions of a profile"; }

  std::string doc() override {
    return
#include "profile-wipe-history.md"
        ;
  }

  void run(ref<store_t> store) override {
    if (min_age) {
      auto t = parse_older_than_time_spec(*min_age);
      delete_generations_older_than(*profile, t, dry_run);
    } else {
      delete_old_generations(*profile, dry_run);
    }
  }
};

struct cmd_profile_t : NixMultiCommand {
  cmd_profile_t()
      : NixMultiCommand(
            "profile",
            {
                {"add", []() { return make_ref<cmd_profile_add_t>(); }},
                {"remove", []() { return make_ref<cmd_profile_remove_t>(); }},
                {"upgrade", []() { return make_ref<cmd_profile_upgrade_t>(); }},
                {"list", []() { return make_ref<cmd_profile_list_t>(); }},
                {"diff-closures", []() { return make_ref<cmd_profile_diff_closures_t>(); }},
                {"history", []() { return make_ref<cmd_profile_history_t>(); }},
                {"rollback", []() { return make_ref<cmd_profile_rollback_t>(); }},
                {"wipe-history", []() { return make_ref<cmd_profile_wipe_history_t>(); }},
            }) {
    get_aliases() = {
        {"install", {alias_status_t::deprecated, {"add"}}},
    };
  }

  std::string description() override { return "manage Nix profiles"; }

  std::string doc() override {
    return
#include "profile.md"
        ;
  }
};

static auto r_cmd_profile = registerCommand<cmd_profile_t>("profile");

} // namespace nix
