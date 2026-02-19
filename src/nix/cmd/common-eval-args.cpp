#include "nix/cmd/common-eval-args.h"

#include "nix/cmd/command.h"
#include "nix/cmd/compatibility-settings.h"
#include "nix/expr/eval-settings.h"
#include "nix/expr/eval.h"
#include "nix/fetchers/fetch-settings.h"
#include "nix/fetchers/fetch-to-store.h"
#include "nix/fetchers/fetchers.h"
#include "nix/fetchers/registry.h"
#include "nix/fetchers/tarball.h"
#include "nix/flake/flakeref.h"
#include "nix/flake/settings.h"
#include "nix/main/shared.h"
#include "nix/store/filetransfer.h"
#include "nix/store/globals.h"
#include "nix/store/store-open.h"
#include "nix/util/config-global.h"

namespace nix {

EvalSettings evalSettings{
    settings.readOnlyMode,
    {
        {
            "flake",
            [](EvalState& state, std::string_view rest) {
              // FIXME `parseFlakeRef` should take a `std::string_view`.
              auto flakeRef = parseFlakeRef(fetchSettings, std::string{rest}, {}, true, false);
              debug("fetching flake search path element '%s''", rest);
              auto [accessor, lockedRef] = flakeRef.resolve(fetchSettings, *state.store)
                                               .lazyFetch(fetchSettings, *state.store);
              auto storePath =
                  nix::fetchToStore(state.fetchSettings, *state.store, source_path_t(accessor),
                                    FetchMode::Copy, lockedRef.input.getName());
              state.allowPath(storePath);
              return state.storePath(storePath);
            },
        },
    },
};

static global_config_t::Register rEvalSettings(&evalSettings);

flake::settings_t flakeSettings;

static global_config_t::Register rFlakeSettings(&flakeSettings);

CompatibilitySettings compatibilitySettings{};

static global_config_t::Register rCompatibilitySettings(&compatibilitySettings);

MixEvalArgs::MixEvalArgs() {
  addFlag({
      .longName = "arg",
      .description = "Pass the value *expr* as the argument *name* to Nix functions.",
      .category = category,
      .labels = {"name", "expr"},
      .handler = {[&](std::string name, std::string expr) {
        autoArgs.insert_or_assign(name, AutoArg{AutoArgExpr{expr}});
      }},
  });

  addFlag({
      .longName = "argstr",
      .description = "Pass the string *string* as the argument *name* to Nix functions.",
      .category = category,
      .labels = {"name", "string"},
      .handler = {[&](std::string name, std::string s) {
        autoArgs.insert_or_assign(name, AutoArg{AutoArgString{s}});
      }},
  });

  addFlag({
      .longName = "arg-from-file",
      .description = "Pass the contents of file *path* as the argument *name* to Nix functions.",
      .category = category,
      .labels = {"name", "path"},
      .handler = {[&](std::string name, std::string path) {
        autoArgs.insert_or_assign(name, AutoArg{AutoArgFile{path}});
      }},
      .completer = completePath,
  });

  addFlag({
      .longName = "arg-from-stdin",
      .description = "Pass the contents of stdin as the argument *name* to Nix functions.",
      .category = category,
      .labels = {"name"},
      .handler = {[&](std::string name) {
        autoArgs.insert_or_assign(name, AutoArg{AutoArgStdin{}});
      }},
  });

  addFlag({
      .longName = "include",
      .shortName = 'I',
      .description = R"(
  Add *path* to search path entries used to resolve [lookup paths](@docroot@/language/constructs/lookup-path.md)

  This option may be given multiple times.

  Paths added through `-I` take precedence over the [`nix-path` configuration setting](@docroot@/command-ref/conf-file.md#conf-nix-path) and the [`NIX_PATH` environment variable](@docroot@/command-ref/env-common.md#env-NIX_PATH).
  )",
      .category = category,
      .labels = {"path"},
      .handler = {[&](std::string s) {
        lookupPath.elements.emplace_back(LookupPath::Elem::parse(s));
      }},
  });

  addFlag({
      .longName = "impure",
      .description = "Allow access to mutable paths and repositories.",
      .category = category,
      .handler = {[&]() { evalSettings.pureEval = false; }},
  });

  addFlag({
      .longName = "override-flake",
      .description = "Override the flake registries, redirecting *original-ref* to *resolved-ref*.",
      .category = category,
      .labels = {"original-ref", "resolved-ref"},
      .handler = {[&](std::string _from, std::string _to) {
        auto from = parseFlakeRef(fetchSettings, _from, std::filesystem::current_path().string());
        auto to = parseFlakeRef(fetchSettings, _to, std::filesystem::current_path().string());
        fetchers::Attrs extraAttrs;
        if (to.subdir != "")
          extraAttrs["dir"] = to.subdir;
        fetchers::overrideRegistry(from.input, to.input, extraAttrs);
      }},
      .completer = {[&](add_completions_t& completions, size_t, std::string_view prefix) {
        completeFlakeRef(completions, openStore(), prefix);
      }},
  });

  addFlag({
      .longName = "eval-store",
      .description =
          R"(
            The [URL of the Nix store](@docroot@/store/types/index.md#store-url-format)
            to use for evaluation, i.e. to store derivations (`.drv` files) and inputs referenced by them.
          )",
      .category = category,
      .labels = {"store-url"},
      .handler = {&evalStoreUrl},
  });
}

Bindings* MixEvalArgs::getAutoArgs(EvalState& state) {
  auto res = state.buildBindings(autoArgs.size());
  for (auto& [name, arg] : autoArgs) {
    auto v = state.allocValue();
    std::visit(
        overloaded{
            [&](const AutoArgExpr& arg) {
              state.mkThunk_(
                  *v, state.parseExprFromString(
                          arg.expr, compatibilitySettings.nixShellShebangArgumentsRelativeToScript
                                        ? state.rootPath(absPath(getCommandBaseDir()).string())
                                        : state.rootPath(".")));
            },
            [&](const AutoArgString& arg) { v->mkString(arg.s, state.mem); },
            [&](const AutoArgFile& arg) { v->mkString(readFile(arg.path.string()), state.mem); },
            [&](const AutoArgStdin& arg) { v->mkString(readFile(STDIN_FILENO), state.mem); }},
        arg);
    res.insert(state.symbols.create(name), v);
  }
  return res.finish();
}

source_path_t lookupFileArg(EvalState& state, std::string_view s,
                         const std::filesystem::path* baseDir) {
  if (EvalSettings::isPseudoUrl(s)) {
    auto accessor = fetchers::downloadTarball(*state.store, state.fetchSettings,
                                              EvalSettings::resolvePseudoUrl(s));
    auto storePath =
        fetchToStore(state.fetchSettings, *state.store, source_path_t(accessor), FetchMode::Copy);
    return state.storePath(storePath);
  }

  else if (hasPrefix(s, "flake:")) {
    auto flakeRef = parseFlakeRef(fetchSettings, std::string(s.substr(6)), {}, true, false);
    auto [accessor, lockedRef] =
        flakeRef.resolve(fetchSettings, *state.store).lazyFetch(fetchSettings, *state.store);
    auto storePath = nix::fetchToStore(state.fetchSettings, *state.store, source_path_t(accessor),
                                       FetchMode::Copy, lockedRef.input.getName());
    state.allowPath(storePath);
    return state.storePath(storePath);
  }

  else if (s.size() > 2 && s.at(0) == '<' && s.at(s.size() - 1) == '>') {
    // Should perhaps be a `CanonPath`?
    std::string p(s.substr(1, s.size() - 2));
    return state.findFile(p);
  }

  else
    return state.rootPath(absPath(std::filesystem::path{s}, baseDir).string());
}

} // namespace nix
