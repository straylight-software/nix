#include <fstream>
#include <regex>

#include <nlohmann/json.hpp>

#include "nix/cmd/command-installable-value.h"
#include "nix/expr/attr-path.h"
#include "nix/expr/eval-cache.h"
#include "nix/expr/eval-inline.h"
#include "nix/expr/eval-settings.h"
#include "nix/expr/eval.h"
#include "nix/expr/get-drvs.h"
#include "nix/expr/parallel-eval.h"
#include "nix/main/common-args.h"
#include "nix/main/shared.h"
#include "nix/store/globals.h"
#include "nix/store/names.h"
#include "nix/util/hilite.h"
#include "nix/util/strings-inline.h"
#include "nix/util/strings.h"

using namespace nix;
using json = nlohmann::json;

std::string wrap(std::string prefix, std::string s) {
  return concat_strings(prefix, s, ANSI_NORMAL);
}

struct cmd_search_t : InstallableValueCommand, MixJSON {
  std::vector<std::string> res;
  std::vector<std::string> exclude_res;

  cmd_search_t() {
    expect_args("regex", &res);
    add_flag(flag_t{
        .long_name = "exclude",
        .short_name = 'e',
        .description = "Hide packages whose attribute path, name or description contain *regex*.",
        .labels = {"regex"},
        .handler = {[this](std::string s) { exclude_res.push_back(s); }},
    });
  }

  std::string description() override { return "search for packages"; }

  std::string doc() override {
    return
#include "search.md"
        ;
  }

  strings_t getDefaultFlakeAttrPaths() override {
    return {"packages." + settings.thisSystem.get(), "legacyPackages." + settings.thisSystem.get()};
  }

  void run(ref<store_t> store, ref<InstallableValue> installable) override {
    settings.readOnlyMode = true;
    eval_settings.enableImportFromDerivation.set_default(false);

    // Recommend "^" here instead of ".*" due to differences in resulting highlighting
    if (res.empty())
      throw UsageError("Must provide at least one regex! To match all packages, use '%s'.",
                       "nix search <installable> ^");

    std::vector<std::regex> regexes;
    std::vector<std::regex> excludeRegexes;
    regexes.reserve(res.size());
    excludeRegexes.reserve(exclude_res.size());

    for (auto& re : res)
      regexes.push_back(std::regex(re, std::regex::extended | std::regex::icase));

    for (auto& re : exclude_res)
      excludeRegexes.emplace_back(re, std::regex::extended | std::regex::icase);

    auto state = getEvalState();

    std::optional<sync_t<nlohmann::json>> jsonOut;
    if (json)
      jsonOut.emplace(json::object());

    std::atomic<uint64_t> results = 0;

    FutureVector futures(*state->executor);

    std::function<void(eval_cache::AttrCursor & cursor, const AttrPath& attr_path,
                       bool initialRecurse)>
        visit;

    visit = [&](eval_cache::AttrCursor& cursor, const AttrPath& attr_path, bool initialRecurse) {
      auto attrPathS = state->symbols.resolve({attr_path});
      auto attrPathStr = attr_path.to_string(*state);

      /*
      activity_t act(*logger, lvl_info, act_unknown, fmt("evaluating '%s'", attrPathStr));
      */
      try {
        auto recurse = [&]() {
          std::vector<std::pair<Executor::work_t, uint8_t>> work;
          for (const auto& attr : cursor.getAttrs()) {
            auto cursor2 = cursor.get_attr(state->symbols[attr]);
            auto attrPath2(attr_path);
            attrPath2.push_back(attr);
            work.emplace_back([cursor2, attrPath2, visit]() { visit(*cursor2, attrPath2, false); },
                              std::string_view(state->symbols[attr]).find("Packages") !=
                                      std::string_view::npos
                                  ? 0
                                  : 2);
          }
          futures.spawn(std::move(work));
        };

        if (cursor.is_derivation()) {
          DrvName name(cursor.get_attr(state->s.name)->get_string());

          auto aMeta = cursor.maybeGetAttr(state->s.meta);
          auto aDescription = aMeta ? aMeta->maybeGetAttr(state->s.description) : nullptr;
          auto description = aDescription ? aDescription->get_string() : "";
          std::replace(description.begin(), description.end(), '\n', ' ');

          std::vector<std::smatch> attrPathMatches;
          std::vector<std::smatch> descriptionMatches;
          std::vector<std::smatch> nameMatches;
          bool found = false;

          for (auto& regex : excludeRegexes) {
            if (std::regex_search(attrPathStr, regex) || std::regex_search(name.name, regex) ||
                std::regex_search(description, regex))
              return;
          }

          for (auto& regex : regexes) {
            found = false;
            auto addAll = [&found](std::sregex_iterator it, std::vector<std::smatch>& vec) {
              const auto end = std::sregex_iterator();
              while (it != end) {
                vec.push_back(*it++);
                found = true;
              }
            };

            addAll(std::sregex_iterator(attrPathStr.begin(), attrPathStr.end(), regex),
                   attrPathMatches);
            addAll(std::sregex_iterator(name.name.begin(), name.name.end(), regex), nameMatches);
            addAll(std::sregex_iterator(description.begin(), description.end(), regex),
                   descriptionMatches);

            if (!found)
              break;
          }

          if (found) {
            results++;
            if (json) {
              (*jsonOut->lock())[attrPathStr] = {
                  {"pname", name.name},
                  {"version", name.version},
                  {"description", description},
              };
            } else {
              auto out = fmt("%s* %s%s", results > 1 ? "\n" : "",
                             wrap("\e[0;1m", hilite_matches(attrPathStr, attrPathMatches,
                                                            ANSI_GREEN, "\e[0;1m")),
                             optional_bracket(" (", name.version, ")"));
              if (description != "")
                out += fmt("\n  %s", hilite_matches(description, descriptionMatches, ANSI_GREEN,
                                                    ANSI_NORMAL));
              logger->cout(out);
            }
          }
        }

        else if (attr_path.size() == 0 ||
                 (attrPathS[0] == "legacyPackages" && attr_path.size() <= 2) ||
                 (attrPathS[0] == "packages" && attr_path.size() <= 2))
          recurse();

        else if (initialRecurse)
          recurse();

        else if (attrPathS[0] == "legacyPackages" && attr_path.size() > 2) {
          auto attr = cursor.maybeGetAttr(state->s.recurseForDerivations);
          if (attr && attr->getBool())
            recurse();
        }

      } catch (EvalError& e) {
        if (!(attr_path.size() > 0 && attrPathS[0] == "legacyPackages"))
          throw;
      }
    };

    std::vector<std::pair<Executor::work_t, uint8_t>> work;
    for (auto& cursor : installable->getCursors(*state)) {
      work.emplace_back([cursor, visit]() { visit(*cursor, cursor->getAttrPath(), true); }, 1);
    }

    futures.spawn(std::move(work));
    futures.finishAll();

    if (json)
      printJSON(*(jsonOut->lock()));

    if (!json && !results)
      throw Error("no results for the given search term(s)!");

    notice("Found %d matching packages.", results);
  }
};

static auto r_cmd_search = registerCommand<cmd_search_t>("search");
