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

eval_settings_t eval_settings{
    settings.readOnlyMode,
    {
        {
            "flake",
            [](eval_state_t& state, std::string_view rest) {
              // FIXME `parseFlakeRef` should take a `std::string_view`.
              auto flake_ref = parse_flake_ref(fetch_settings, std::string{rest}, {}, true, false);
              debug("fetching flake search path element '%s''", rest);
              auto [accessor, locked_ref] = flake_ref.resolve(fetch_settings, *state.store)
                                               .lazyFetch(fetch_settings, *state.store);
              auto store_path =
                  nix::fetch_to_store(state.fetch_settings, *state.store, source_path_t(accessor),
                                    FetchMode::Copy, locked_ref.input.get_name());
              state.allowPath(store_path);
              return state.store_path(store_path);
            },
        },
    },
};

static global_config_t::Register r_eval_settings(&eval_settings);

flake::settings_t flake_settings;

static global_config_t::Register r_flake_settings(&flake_settings);

CompatibilitySettings compatibility_settings{};

static global_config_t::Register r_compatibility_settings(&compatibility_settings);

MixEvalArgs::MixEvalArgs() {
  add_flag({
      .long_name = "arg",
      .description = "Pass the value *expr* as the argument *name* to Nix functions.",
      .category = category,
      .labels = {"name", "expr"},
      .handler = {[&](std::string name, std::string expr) {
        auto_args.insert_or_assign(name, AutoArg{AutoArgExpr{expr}});
      }},
  });

  add_flag({
      .long_name = "argstr",
      .description = "Pass the string *string* as the argument *name* to Nix functions.",
      .category = category,
      .labels = {"name", "string"},
      .handler = {[&](std::string name, std::string s) {
        auto_args.insert_or_assign(name, AutoArg{AutoArgString{s}});
      }},
  });

  add_flag({
      .long_name = "arg-from-file",
      .description = "Pass the contents of file *path* as the argument *name* to Nix functions.",
      .category = category,
      .labels = {"name", "path"},
      .handler = {[&](std::string name, std::string path) {
        auto_args.insert_or_assign(name, AutoArg{AutoArgFile{path}});
      }},
      .completer = complete_path,
  });

  add_flag({
      .long_name = "arg-from-stdin",
      .description = "Pass the contents of stdin as the argument *name* to Nix functions.",
      .category = category,
      .labels = {"name"},
      .handler = {[&](std::string name) {
        auto_args.insert_or_assign(name, AutoArg{AutoArgStdin{}});
      }},
  });

  add_flag({
      .long_name = "include",
      .short_name = 'I',
      .description = R"(
  Add *path* to search path entries used to resolve [lookup paths](@docroot@/language/constructs/lookup-path.md)

  This option may be given multiple times.

  Paths added through `-I` take precedence over the [`nix-path` configuration setting](@docroot@/command-ref/conf-file.md#conf-nix-path) and the [`NIX_PATH` environment variable](@docroot@/command-ref/env-common.md#env-NIX_PATH).
  )",
      .category = category,
      .labels = {"path"},
      .handler = {[&](std::string s) {
        lookup_path.elements.emplace_back(LookupPath::Elem::parse(s));
      }},
  });

  add_flag({
      .long_name = "impure",
      .description = "Allow access to mutable paths and repositories.",
      .category = category,
      .handler = {[&]() { eval_settings.pureEval = false; }},
  });

  add_flag({
      .long_name = "override-flake",
      .description = "Override the flake registries, redirecting *original-ref* to *resolved-ref*.",
      .category = category,
      .labels = {"original-ref", "resolved-ref"},
      .handler = {[&](std::string _from, std::string _to) {
        auto from = parse_flake_ref(fetch_settings, _from, std::filesystem::current_path().string());
        auto to = parse_flake_ref(fetch_settings, _to, std::filesystem::current_path().string());
        fetchers::Attrs extra_attrs;
        if (to.subdir != "")
          extra_attrs["dir"] = to.subdir;
        fetchers::override_registry(from.input, to.input, extra_attrs);
      }},
      .completer = {[&](add_completions_t& completions, size_t, std::string_view prefix) {
        complete_flake_ref(completions, open_store(), prefix);
      }},
  });

  add_flag({
      .long_name = "eval-store",
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

bindings_t* MixEvalArgs::getAutoArgs(eval_state_t& state) {
  auto res = state.buildBindings(auto_args.size());
  for (auto& [name, arg] : auto_args) {
    auto v = state.allocValue();
    std::visit(
        overloaded{
            [&](const AutoArgExpr& arg) {
              state.mkThunk_(
                  *v, state.parseExprFromString(
                          arg.expr, compatibility_settings.nixShellShebangArgumentsRelativeToScript
                                        ? state.root_path(abs_path(get_command_base_dir()).string())
                                        : state.root_path(".")));
            },
            [&](const AutoArgString& arg) { v->mk_string(arg.s, state.mem); },
            [&](const AutoArgFile& arg) { v->mk_string(read_file(arg.path.string()), state.mem); },
            [&](const AutoArgStdin& arg) { v->mk_string(read_file(STDIN_FILENO), state.mem); }},
        arg);
    res.insert(state.symbols.create(name), v);
  }
  return res.finish();
}

source_path_t lookup_file_arg(eval_state_t& state, std::string_view s,
                         const std::filesystem::path* base_dir) {
  if (eval_settings_t::isPseudoUrl(s)) {
    auto accessor = fetchers::download_tarball(*state.store, state.fetch_settings,
                                              eval_settings_t::resolvePseudoUrl(s));
    auto store_path =
        fetch_to_store(state.fetch_settings, *state.store, source_path_t(accessor), FetchMode::Copy);
    return state.store_path(store_path);
  }

  else if (has_prefix(s, "flake:")) {
    auto flake_ref = parse_flake_ref(fetch_settings, std::string(s.substr(6)), {}, true, false);
    auto [accessor, locked_ref] =
        flake_ref.resolve(fetch_settings, *state.store).lazyFetch(fetch_settings, *state.store);
    auto store_path = nix::fetch_to_store(state.fetch_settings, *state.store, source_path_t(accessor),
                                       FetchMode::Copy, locked_ref.input.get_name());
    state.allowPath(store_path);
    return state.store_path(store_path);
  }

  else if (s.size() > 2 && s.at(0) == '<' && s.at(s.size() - 1) == '>') {
    // Should perhaps be a `CanonPath`?
    std::string p(s.substr(1, s.size() - 2));
    return state.findFile(p);
  }

  else
    return state.root_path(abs_path(std::filesystem::path{s}, base_dir).string());
}

} // namespace nix
