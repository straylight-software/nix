#include <regex>

#include "nix/cmd/command.h"
#include "nix/main/common-args.h"
#include "nix/main/shared.h"
#include "nix/store/names.h"
#include "nix/store/store-api.h"
#include "nix/util/strings.h"

namespace nix {

namespace {

struct Info {
  std::string output_name;
};

} // namespace

// name -> version -> store paths
typedef std::map<std::string, std::map<std::string, std::map<store_path_t, Info>>> GroupedPaths;

GroupedPaths get_closure_info(ref<store_t> store, const store_path_t& toplevel) {
  store_path_set_t closure;
  store->computeFSClosure({toplevel}, closure);

  GroupedPaths grouped_paths;

  for (auto const& path : closure) {
    /* Strip the output name. Unfortunately this is ambiguous (we
       can't distinguish between output names like "bin" and
       version suffixes like "unstable"). */
    static std::regex regex("(.*)-([a-z]+|lib32|lib64)");
    std::cmatch match;
    std::string name{path.name()};
    std::string_view const origName = path.name();
    std::string output_name;

    if (std::regex_match(origName.begin(), origName.end(), match, regex)) {
      name = match[1];
      output_name = match[2];
    }

    DrvName drv_name(name);
    grouped_paths[drv_name.name][drv_name.version].emplace(path, Info{.output_name = output_name});
  }

  return grouped_paths;
}

std::string show_versions(const string_set_t& versions) {
  if (versions.empty())
    return "(absent)";
  string_set_t versions2;
  for (auto& version : versions)
    versions2.insert(version.empty() ? "(no version)" : version);
  return concat_strings_sep(", ", versions2);
}

void print_closure_diff(ref<store_t> store, const store_path_t& before_path,
                        const store_path_t& after_path, std::string_view indent) {
  auto before_closure = get_closure_info(store, before_path);
  auto after_closure = get_closure_info(store, after_path);

  string_set_t all_names;
  for (auto& [name, _] : before_closure)
    all_names.insert(name);
  for (auto& [name, _] : after_closure)
    all_names.insert(name);

  for (auto& name : all_names) {
    auto& beforeVersions = before_closure[name];
    auto& afterVersions = after_closure[name];

    auto totalSize = [&](const std::map<std::string, std::map<store_path_t, Info>>& versions) {
      uint64_t sum = 0;
      for (auto& [_, paths] : versions)
        for (auto& [path, _] : paths)
          sum += store->queryPathInfo(path)->nar_size;
      return sum;
    };

    auto beforeSize = totalSize(beforeVersions);
    auto afterSize = totalSize(afterVersions);
    auto sizeDelta = (int64_t)afterSize - (int64_t)beforeSize;
    auto showDelta = std::abs(sizeDelta) >= 8 * 1024;

    string_set_t removed, unchanged;
    for (auto& [version, _] : beforeVersions)
      if (!afterVersions.count(version))
        removed.insert(version);
      else
        unchanged.insert(version);

    string_set_t added;
    for (auto& [version, _] : afterVersions)
      if (!beforeVersions.count(version))
        added.insert(version);

    if (showDelta || !removed.empty() || !added.empty()) {
      std::vector<std::string> items;
      if (!removed.empty() && !added.empty()) {
        items.push_back(fmt("%s → %s", show_versions(removed), show_versions(added)));
      } else if (!removed.empty()) {
        items.push_back(fmt("%s removed", show_versions(removed)));
      } else if (!added.empty()) {
        items.push_back(fmt("%s added", show_versions(added)));
      }
      if (showDelta)
        items.push_back(
            fmt("%s%s" ANSI_NORMAL, sizeDelta > 0 ? ANSI_RED : ANSI_GREEN, render_size(sizeDelta)));
      logger->cout("%s%s: %s", indent, name, concat_strings_sep(", ", items));
    }
  }
}

} // namespace nix

struct cmd_diff_closures_t : nix::SourceExprCommand, nix::MixOperateOnOptions {
  std::string _before, _after;

  cmd_diff_closures_t() {
    expect_arg("before", &_before);
    expect_arg("after", &_after);
  }

  std::string description() override {
    return "show what packages and versions were added and removed between two closures";
  }

  std::string doc() override {
    return
#include "diff-closures.md"
        ;
  }

  void run(nix::ref<nix::store_t> store) override {
    auto before = parseInstallable(store, _before);
    auto before_path = nix::Installable::toStorePath(getEvalStore(), store, nix::Realise::Outputs,
                                                     operateOn, before);
    auto after = parseInstallable(store, _after);
    auto after_path = nix::Installable::toStorePath(getEvalStore(), store, nix::Realise::Outputs,
                                                    operateOn, after);
    nix::print_closure_diff(store, before_path, after_path, "");
  }
};

static auto r_cmd_diff_closures =
    nix::registerCommand2<cmd_diff_closures_t>({"store", "diff-closures"});
