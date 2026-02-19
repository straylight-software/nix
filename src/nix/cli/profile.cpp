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

using namespace nix;

struct profile_element_source_t {
  FlakeRef originalRef;
  // FIXME: record original attrpath.
  FlakeRef lockedRef;
  std::string attrPath;
  ExtendedOutputsSpec outputs;

  // TODO libc++ 16 (used by darwin) missing `std::set::operator <=>`, can't do yet.
  // auto operator <=> (const ProfileElementSource & other) const
  auto operator<(const profile_element_source_t& other) const {
    return std::tuple(originalRef.to_string(), attrPath, outputs) <
           std::tuple(other.originalRef.to_string(), other.attrPath, other.outputs);
  }

  std::string to_string() const {
    return fmt("%s#%s%s", originalRef, attrPath, outputs.to_string());
  }
};

const int defaultPriority = 5;

struct profile_element_t {
  StorePathSet storePaths;
  std::optional<profile_element_source_t> source;
  bool active = true;
  int priority = defaultPriority;

  std::string identifier() const {
    if (source)
      return source->to_string();
    string_set_t names;
    for (auto& path : storePaths)
      names.insert(DrvName(path.name()).name);
    return dropEmptyInitThenConcatStringsSep(", ", names);
  }

  /**
   * Return a string representing an installable corresponding to the current
   * element, either a flakeref or a plain store path
   */
  string_set_t toInstallables(Store& store) {
    if (source)
      return {source->to_string()};
    string_set_t rawPaths;
    for (auto& path : storePaths)
      rawPaths.insert(store.printStorePath(path));
    return rawPaths;
  }

  std::string versions() const {
    string_set_t versions;
    for (auto& path : storePaths)
      versions.insert(DrvName(path.name()).version);
    return showVersions(versions);
  }

  void updateStorePaths(ref<Store> evalStore, ref<Store> store, const BuiltPaths& builtPaths) {
    storePaths.clear();
    for (auto& buildable : builtPaths) {
      std::visit(overloaded{
                     [&](const BuiltPath::opaque_t& bo) { storePaths.insert(bo.path); },
                     [&](const BuiltPath::Built& bfd) {
                       for (auto& output : bfd.outputs)
                         storePaths.insert(output.second);
                     },
                 },
                 buildable.raw());
    }
  }
};

std::string getNameFromElement(const profile_element_t& element) {
  std::optional<std::string> result = std::nullopt;
  if (element.source) {
    // Seems to be for Flake URLs
    result = getNameFromURL(parseURL(element.source->to_string(), /*lenient=*/true));
  }
  return result.value_or(element.identifier());
}

struct profile_manifest_t {
  using ProfileElementName = std::string;

  std::map<ProfileElementName, profile_element_t> elements;

  profile_manifest_t() {}

  profile_manifest_t(EvalState& state, const std::filesystem::path& profile) {
    auto manifestPath = profile / "manifest.json";

    if (std::filesystem::exists(manifestPath)) {
      auto json = nlohmann::json::parse(readFile(manifestPath.string()));

      auto version = json.value("version", 0);
      std::string sUrl;
      std::string sOriginalUrl;
      switch (version) {
        case 1:
          sUrl = "uri";
          sOriginalUrl = "originalUri";
          break;
        case 2:
        case 3:
          sUrl = "url";
          sOriginalUrl = "originalUrl";
          break;
        default:
          throw Error("profile manifest '%s' has unsupported version %d", manifestPath, version);
      }

      auto elems = json["elements"];
      for (auto& elem : elems.items()) {
        auto& e = elem.value();
        profile_element_t element;
        for (auto& p : e["storePaths"])
          element.storePaths.insert(state.store->parseStorePath((std::string)p));
        element.active = e["active"];
        if (e.contains("priority")) {
          element.priority = e["priority"];
        }
        if (e.value(sUrl, "") != "") {
          element.source = profile_element_source_t{
              parseFlakeRef(fetchSettings, e[sOriginalUrl]), parseFlakeRef(fetchSettings, e[sUrl]),
              e["attrPath"], e["outputs"].get<ExtendedOutputsSpec>()};
        }

        std::string name = [&] {
          if (elems.is_object())
            return elem.key();
          if (element.source) {
            if (auto optName =
                    getNameFromURL(parseURL(element.source->to_string(), /*lenient=*/true)))
              return *optName;
          }
          return element.identifier();
        }();

        addElement(name, std::move(element));
      }
    }

    else if (std::filesystem::exists(profile / "manifest.nix")) {
      // FIXME: needed because of pure mode; ugly.
      state.allowPath(state.store->followLinksToStorePath(profile.string()));
      state.allowPath(state.store->followLinksToStorePath((profile / "manifest.nix").string()));

      auto packageInfos = queryInstalled(state, state.store->followLinksToStore(profile.string()));

      for (auto& packageInfo : packageInfos) {
        profile_element_t element;
        element.storePaths = {packageInfo.queryOutPath()};
        addElement(std::move(element));
      }
    }
  }

  void addElement(std::string_view nameCandidate, profile_element_t element) {
    std::string finalName(nameCandidate);
    for (int i = 1; elements.contains(finalName); ++i)
      finalName = nameCandidate + "-" + std::to_string(i);

    elements.insert_or_assign(finalName, std::move(element));
  }

  void addElement(profile_element_t element) {
    auto name = getNameFromElement(element);
    addElement(name, std::move(element));
  }

  nlohmann::json toJSON(Store& store) const {
    auto es = nlohmann::json::object();
    for (auto& [name, element] : elements) {
      auto paths = nlohmann::json::array();
      for (auto& path : element.storePaths)
        paths.push_back(store.printStorePath(path));
      nlohmann::json obj;
      obj["storePaths"] = paths;
      obj["active"] = element.active;
      obj["priority"] = element.priority;
      if (element.source) {
        obj["originalUrl"] = element.source->originalRef.to_string();
        obj["url"] = element.source->lockedRef.to_string();
        obj["attrPath"] = element.source->attrPath;
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

  StorePath build(ref<Store> store) {
    auto tempDir = createTempDir();

    StorePathSet references;

    Packages pkgs;
    for (auto& [name, element] : elements) {
      for (auto& path : element.storePaths) {
        if (element.active)
          pkgs.emplace_back(store->printStorePath(path), true, element.priority);
        references.insert(path);
      }
    }

    buildProfile(tempDir.string(), std::move(pkgs));

    writeFile(tempDir / "manifest.json", toJSON(*store).dump());

    /* Add the symlink tree to the store. */
    string_sink_t sink;
    dumpPath(tempDir.string(), sink);

    auto narHash = hashString(hash_algorithm_t::SHA256, sink.s);

    auto info = ValidPathInfo::makeFromCA(*store, "profile",
                                          FixedOutputInfo{
                                              .method = file_ingestion_method_t::NixArchive,
                                              .hash = narHash,
                                              .references =
                                                  {
                                                      .others = std::move(references),
                                                      // profiles never refer to themselves
                                                      .self = false,
                                                  },
                                          },
                                          narHash);
    info.narSize = sink.s.size();

    string_source_t source(sink.s);
    store->addToStore(info, source);

    return std::move(info.path);
  }

  static void printDiff(const profile_manifest_t& prev, const profile_manifest_t& cur,
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

    if (!changes)
      logger->cout("%sNo changes.", indent);
  }
};

static std::map<Installable*, std::pair<BuiltPaths, ref<ExtraPathInfo>>>
builtPathsPerInstallable(const std::vector<InstallableWithBuildResult>& builtPaths) {
  std::map<Installable*, std::pair<BuiltPaths, ref<ExtraPathInfo>>> res;
  for (auto& b : builtPaths) {
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
    addFlag({
        .longName = "priority",
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

  void run(ref<Store> store, Installables&& installables) override {
    profile_manifest_t manifest(*getEvalState(), *profile);

    auto buildResults =
        Installable::build2(getEvalStore(), store, Realise::Outputs, installables, bmNormal);
    Installable::throwBuildErrors(buildResults, *store);

    auto builtPaths = builtPathsPerInstallable(buildResults);

    for (auto& installable : installables) {
      profile_element_t element;

      auto iter = builtPaths.find(&*installable);
      if (iter == builtPaths.end())
        continue;
      auto& [res, info] = iter->second;

      if (auto* info2 = dynamic_cast<ExtraPathInfoFlake*>(&*info)) {
        element.source = profile_element_source_t{
            .originalRef = info2->flake.originalRef,
            .lockedRef = info2->flake.lockedRef,
            .attrPath = info2->value.attrPath,
            .outputs = info2->value.extendedOutputsSpec,
        };
      }

      // If --priority was specified we want to override the
      // priority of the installable.
      element.priority = priority ? *priority : ({
        auto* info2 = dynamic_cast<ExtraPathInfoValue*>(&*info);
        info2 ? info2->value.priority.value_or(defaultPriority) : defaultPriority;
      });

      element.updateStorePaths(getEvalStore(), store, res);

      auto elementName = getNameFromElement(element);

      // Check if the element already exists.
      auto existingPair = manifest.elements.find(elementName);
      if (existingPair != manifest.elements.end()) {
        auto existingElement = existingPair->second;
        auto existingSource = existingElement.source;
        auto elementSource = element.source;
        if (existingSource && elementSource && existingElement.priority == element.priority &&
            existingSource->originalRef == elementSource->originalRef &&
            existingSource->attrPath == elementSource->attrPath) {
          warn("'%s' is already added", elementName);
          continue;
        }
      }

      manifest.addElement(elementName, std::move(element));
    }

    try {
      updateProfile(manifest.build(store));
    } catch (BuildEnvFileConflictError& conflictError) {
      // FIXME use C++20 std::ranges once macOS has it
      //       See
      //       https://github.com/NixOS/nix/compare/3efa476c5439f8f6c1968a6ba20a31d1239c2f04..1fe5d172ece51a619e879c4b86f603d9495cc102
      auto findRefByFilePath = [&]<typename Iterator>(Iterator begin, Iterator end) {
        for (auto it = begin; it != end; it++) {
          auto& [name, profileElement] = *it;
          for (auto& storePath : profileElement.storePaths) {
            if (conflictError.fileA.starts_with(store->printStorePath(storePath))) {
              return std::tuple(conflictError.fileA, name, profileElement.toInstallables(*store));
            }
            if (conflictError.fileB.starts_with(store->printStorePath(storePath))) {
              return std::tuple(conflictError.fileB, name, profileElement.toInstallables(*store));
            }
          }
        }
        throw conflictError;
      };
      // There are 2 conflicting files. We need to find out which one is from the already installed
      // package and which one is the package that is the new package that is being installed. The
      // first matching package is the one that was already installed (original).
      auto [originalConflictingFilePath, originalEntryName, originalConflictingRefs] =
          findRefByFilePath(manifest.elements.begin(), manifest.elements.end());
      // The last matching package is the one that was going to be installed (new).
      auto [newConflictingFilePath, newEntryName, newConflictingRefs] =
          findRefByFilePath(manifest.elements.rbegin(), manifest.elements.rend());

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
                  concatStringsSep(" ", newConflictingRefs), conflictError.priority,
                  conflictError.priority - 1, conflictError.priority + 1);
    }
  }
};

struct matcher_t {
  virtual ~matcher_t() {}

  virtual std::string getTitle() = 0;
  virtual bool matches(const std::string& name, const profile_element_t& element) = 0;
};

struct regex_matcher_t final : public matcher_t {
  std::regex regex;
  std::string pattern;

  regex_matcher_t(const std::string& pattern)
      : regex(pattern, std::regex::extended | std::regex::icase), pattern(pattern) {}

  std::string getTitle() override { return fmt("Regex '%s'", pattern); }

  bool matches(const std::string& name, const profile_element_t& element) override {
    return std::regex_match(element.identifier(), regex);
  }
};

struct store_path_matcher_t final : public matcher_t {
  nix::StorePath storePath;

  store_path_matcher_t(const nix::StorePath& storePath) : storePath(storePath) {}

  std::string getTitle() override { return fmt("Store path '%s'", storePath.to_string()); }

  bool matches(const std::string& name, const profile_element_t& element) override {
    return element.storePaths.count(storePath);
  }
};

struct name_matcher_t final : public matcher_t {
  std::string name;

  name_matcher_t(const std::string& name) : name(name) {}

  std::string getTitle() override { return fmt("Package name '%s'", name); }

  bool matches(const std::string& name, const profile_element_t& element) override {
    return name == this->name;
  }
};

struct all_matcher_t final : public matcher_t {
  std::string getTitle() override { return "--all"; }

  bool matches(const std::string& name, const profile_element_t& element) override { return true; }
};

all_matcher_t all;

class mix_profile_element_matchers_t : virtual Args, virtual StoreCommand {
  std::vector<ref<matcher_t>> _matchers;

public:
  mix_profile_element_matchers_t() {
    addFlag({
        .longName = "all",
        .description = "Match all packages in the profile.",
        .handler = {[this]() {
          _matchers.push_back(
              ref<all_matcher_t>(std::shared_ptr<all_matcher_t>(&all, [](all_matcher_t*) {})));
        }},
    });
    addFlag({
        .longName = "regex",
        .description = "A regular expression to match one or more packages in the profile.",
        .labels = {"pattern"},
        .handler = {[this](std::string arg) { _matchers.push_back(make_ref<regex_matcher_t>(arg)); }},
    });
    expectArgs({.label = "elements",
                .optional = true,
                .handler = {[this](std::vector<std::string> args) {
                  for (auto& arg : args) {
                    if (auto n = string2Int<size_t>(arg)) {
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

  string_set_t getMatchingElementNames(profile_manifest_t& manifest) {
    if (_matchers.empty()) {
      throw UsageError("No packages specified.");
    }

    if (std::find_if(_matchers.begin(), _matchers.end(),
                     [](const ref<matcher_t>& m) { return m.dynamic_pointer_cast<all_matcher_t>(); }) !=
            _matchers.end() &&
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
        warn("%s does not match any packages in the profile.", matcher->getTitle());
      }
    }
    return result;
  }
};

struct cmd_profile_remove_t : virtual EvalCommand, MixDefaultProfile, mix_profile_element_matchers_t {
  std::string description() override { return "remove packages from a profile"; }

  std::string doc() override {
    return
#include "profile-remove.md"
        ;
  }

  void run(ref<Store> store) override {
    profile_manifest_t oldManifest(*getEvalState(), *profile);

    profile_manifest_t newManifest = oldManifest;

    auto matchingElementNames = getMatchingElementNames(oldManifest);

    if (matchingElementNames.empty()) {
      warn("No packages to remove. Use 'nix profile list' to see the current profile.");
      return;
    }

    for (auto& name : matchingElementNames) {
      auto& element = oldManifest.elements[name];
      notice("removing '%s'", element.identifier());
      newManifest.elements.erase(name);
    }

    auto removedCount = oldManifest.elements.size() - newManifest.elements.size();
    printInfo("removed %d packages, kept %d packages", removedCount, newManifest.elements.size());

    updateProfile(newManifest.build(store));
  }
};

struct cmd_profile_upgrade_t : virtual SourceExprCommand, MixDefaultProfile, mix_profile_element_matchers_t {
  std::string description() override { return "upgrade packages using their most recent flake"; }

  std::string doc() override {
    return
#include "profile-upgrade.md"
        ;
  }

  void run(ref<Store> store) override {
    profile_manifest_t manifest(*getEvalState(), *profile);

    Installables installables;
    std::vector<profile_element_t*> elems;

    auto upgradedCount = 0;

    auto matchingElementNames = getMatchingElementNames(manifest);

    if (matchingElementNames.empty()) {
      warn("No packages to upgrade. Use 'nix profile list' to see the current profile.");
      return;
    }

    for (auto& name : matchingElementNames) {
      auto& element = manifest.elements[name];

      if (!element.source) {
        warn("Found package '%s', but it was not added from a flake, so it can't be checked for "
             "upgrades!",
             element.identifier());
        continue;
      }
      if (element.source->originalRef.input.isLocked(getEvalState()->fetchSettings)) {
        warn("Found package '%s', but it was added from a locked flake reference so it can't be "
             "upgraded!",
             element.identifier());
        continue;
      }

      upgradedCount++;

      activity_t act(*logger, lvlChatty, actUnknown,
                   fmt("checking '%s' for updates", element.source->attrPath));

      auto installable = make_ref<InstallableFlake>(
          this, getEvalState(), FlakeRef(element.source->originalRef), "", element.source->outputs,
          strings_t{element.source->attrPath}, strings_t{}, lockFlags);

      auto derivedPaths = installable->toDerivedPaths();
      if (derivedPaths.empty())
        continue;
      auto* infop = dynamic_cast<ExtraPathInfoFlake*>(&*derivedPaths[0].info);
      // `InstallableFlake` should use `ExtraPathInfoFlake`.
      assert(infop);
      auto& info = *infop;

      if (info.flake.lockedRef.input.isLocked(getEvalState()->fetchSettings) &&
          element.source->lockedRef == info.flake.lockedRef)
        continue;

      printInfo("upgrading '%s' from flake '%s' to '%s'", element.source->attrPath,
                element.source->lockedRef, info.flake.lockedRef);

      element.source = profile_element_source_t{
          .originalRef = installable->flakeRef,
          .lockedRef = info.flake.lockedRef,
          .attrPath = info.value.attrPath,
          .outputs = installable->extendedOutputsSpec,
      };

      installables.push_back(installable);
      elems.push_back(&element);
    }

    if (upgradedCount == 0) {
      warn("Found some packages but none of them could be upgraded.");
      return;
    }

    auto buildResults =
        Installable::build2(getEvalStore(), store, Realise::Outputs, installables, bmNormal);
    Installable::throwBuildErrors(buildResults, *store);

    auto builtPaths = builtPathsPerInstallable(buildResults);

    for (size_t i = 0; i < installables.size(); ++i) {
      auto& installable = installables.at(i);
      auto& element = *elems.at(i);
      element.updateStorePaths(getEvalStore(), store, builtPaths.find(&*installable)->second.first);
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

  void run(ref<Store> store) override {
    profile_manifest_t manifest(*getEvalState(), *profile);

    if (json) {
      printJSON(manifest.toJSON(*store));
    } else {
      for (const auto& [i, e] : enumerate(manifest.elements)) {
        auto& [name, element] = e;
        if (i)
          logger->cout("");
        logger->cout("Name:               " ANSI_BOLD "%s" ANSI_NORMAL "%s", name,
                     element.active ? "" : " " ANSI_RED "(inactive)" ANSI_NORMAL);
        if (element.source) {
          logger->cout("Flake attribute:    %s%s", element.source->attrPath,
                       element.source->outputs.to_string());
          logger->cout("Original flake URL: %s", element.source->originalRef.to_string());
          logger->cout("Locked flake URL:   %s", element.source->lockedRef.to_string());
        }
        logger->cout("Store paths:        %s",
                     concatStringsSep(" ", store->printStorePathSet(element.storePaths)));
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

  void run(ref<Store> store) override {
    auto [gens, curGen] = findGenerations(*profile);

    std::optional<Generation> prevGen;
    bool first = true;

    for (auto& gen : gens) {
      if (prevGen) {
        if (!first)
          logger->cout("");
        first = false;
        logger->cout("Version %d -> %d:", prevGen->number, gen.number);
        printClosureDiff(store, store->followLinksToStorePath(prevGen->path.string()),
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

  void run(ref<Store> store) override {
    auto [gens, curGen] = findGenerations(*profile);

    std::optional<std::pair<Generation, profile_manifest_t>> prevGen;
    bool first = true;

    for (auto& gen : gens) {
      profile_manifest_t manifest(*getEvalState(), gen.path);

      if (!first)
        logger->cout("");
      first = false;

      logger->cout(
          "Version %s%d" ANSI_NORMAL " (%s)%s:", gen.number == curGen ? ANSI_GREEN : ANSI_BOLD,
          gen.number, std::put_time(std::gmtime(&gen.creationTime), "%Y-%m-%d"),
          prevGen ? fmt(" <- %d", prevGen->first.number) : "");

      profile_manifest_t::printDiff(prevGen ? prevGen->second : profile_manifest_t(), manifest, "  ");

      prevGen = {gen, std::move(manifest)};
    }
  }
};

struct cmd_profile_rollback_t : virtual StoreCommand, MixDefaultProfile, MixDryRun {
  std::optional<GenerationNumber> version;

  cmd_profile_rollback_t() {
    addFlag({
        .longName = "to",
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

  void run(ref<Store> store) override { switchGeneration(*profile, version, dryRun); }
};

struct cmd_profile_wipe_history_t : virtual StoreCommand, MixDefaultProfile, MixDryRun {
  std::optional<std::string> minAge;

  cmd_profile_wipe_history_t() {
    addFlag({
        .longName = "older-than",
        .description = "Delete versions older than the specified age. *age* "
                       "must be in the format *N*`d`, where *N* denotes a number "
                       "of days.",
        .labels = {"age"},
        .handler = {&minAge},
    });
  }

  std::string description() override { return "delete non-current versions of a profile"; }

  std::string doc() override {
    return
#include "profile-wipe-history.md"
        ;
  }

  void run(ref<Store> store) override {
    if (minAge) {
      auto t = parseOlderThanTimeSpec(*minAge);
      deleteGenerationsOlderThan(*profile, t, dryRun);
    } else
      deleteOldGenerations(*profile, dryRun);
  }
};

struct cmd_profile_t : NixMultiCommand {
  cmd_profile_t()
      : NixMultiCommand("profile",
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
    aliases = {
        {"install", {alias_status_t::Deprecated, {"add"}}},
    };
  }

  std::string description() override { return "manage Nix profiles"; }

  std::string doc() override {
    return
#include "profile.md"
        ;
  }
};

static auto rCmdProfile = registerCommand<cmd_profile_t>("profile");
