#include "nix/cmd/command-installable-value.h"
#include "nix/cmd/installable-flake.h"
#include "nix/expr/eval.h"
#include "nix/fetchers/fetch-settings.h"
#include "nix/main/common-args.h"
#include "nix/main/shared.h"
#include "nix/store/derivations.h"
#include "nix/store/globals.h"
#include "nix/store/outputs-spec.h"
#include "nix/store/store-api.h"
#include "nix/util/config-global.h"

#ifndef _WIN32 // TODO re-enable on Windows
#  include "run.h"
#endif

#include <algorithm>
#include <iterator>
#include <memory>
#include <sstream>

#include <nlohmann/json.hpp>

#include "nix/util/strings.h"

namespace nix::fs {
using namespace std::filesystem;
}

using namespace nix;

struct develop_settings_t : config_t {
  setting_t<std::string> bash_prompt{this, "", "bash-prompt",
                                  "The bash prompt (`PS1`) in `nix develop` shells."};

  setting_t<std::string> bash_prompt_prefix{
      this, "", "bash-prompt-prefix",
      "Prefix prepended to the `PS1` environment variable in `nix develop` shells."};

  setting_t<std::string> bash_prompt_suffix{
      this, "", "bash-prompt-suffix",
      "Suffix appended to the `PS1` environment variable in `nix develop` shells."};
};

static develop_settings_t develop_settings;

static global_config_t::Register r_develop_settings(&develop_settings);

struct build_environment_t {
  struct String {
    bool exported;
    std::string value;

    bool operator==(const String& other) const {
      return exported == other.exported && value == other.value;
    }
  };

  using Array = std::vector<std::string>;

  using Associative = string_map_t;

  using Value = std::variant<String, Array, Associative>;

  std::map<std::string, Value> vars;
  string_map_t bash_functions;
  std::optional<std::pair<std::string, std::string>> structured_attrs;

  static build_environment_t from_json(const nlohmann::json& json) {
    build_environment_t res;

    string_set_t exported;

    for (auto& [name, info] : json["variables"].items()) {
      std::string type = info["type"];
      if (type == "var" || type == "exported")
        res.vars.insert({name, build_environment_t::String{.exported = type == "exported",
                                                        .value = info["value"]}});
      else if (type == "array")
        res.vars.insert({name, (Array)info["value"]});
      else if (type == "associative")
        res.vars.insert({name, (Associative)info["value"]});
    }

    for (auto& [name, def] : json["bashFunctions"].items()) {
      res.bash_functions.insert({name, def});
    }

    if (json.contains("structuredAttrs")) {
      res.structured_attrs = {json["structuredAttrs"][".attrs.json"],
                             json["structuredAttrs"][".attrs.sh"]};
    }

    return res;
  }

  static build_environment_t parse_json(std::string_view in) {
    auto json = nlohmann::json::parse(in);

    return from_json(json);
  }

  nlohmann::json to_json() const {
    auto res = nlohmann::json::object();

    auto vars2 = nlohmann::json::object();
    for (auto& [name, value] : vars) {
      auto info = nlohmann::json::object();
      if (auto str = std::get_if<String>(&value)) {
        info["type"] = str->exported ? "exported" : "var";
        info["value"] = str->value;
      } else if (auto arr = std::get_if<Array>(&value)) {
        info["type"] = "array";
        info["value"] = *arr;
      } else if (auto arr = std::get_if<Associative>(&value)) {
        info["type"] = "associative";
        info["value"] = *arr;
      }
      vars2[name] = std::move(info);
    }
    res["variables"] = std::move(vars2);

    res["bashFunctions"] = bash_functions;

    if (provides_structured_attrs()) {
      auto contents = nlohmann::json::object();
      contents[".attrs.sh"] = get_attrs_sh();
      contents[".attrs.json"] = get_attrs_json();
      res["structuredAttrs"] = std::move(contents);
    }

    assert(build_environment_t::from_json(res) == *this);

    return res;
  }

  bool provides_structured_attrs() const { return structured_attrs.has_value(); }

  std::string get_attrs_json() const {
    assert(provides_structured_attrs());
    return structured_attrs->first;
  }

  std::string get_attrs_sh() const {
    assert(provides_structured_attrs());
    return structured_attrs->second;
  }

  void to_bash(std::ostream& out, const string_set_t& ignore_vars) const {
    for (auto& [name, value] : vars) {
      if (!ignore_vars.count(name)) {
        if (auto str = std::get_if<String>(&value)) {
          out << fmt("%s=%s\n", name, escape_shell_arg_always(str->value));
          if (str->exported)
            out << fmt("export %s\n", name);
        } else if (auto arr = std::get_if<Array>(&value)) {
          out << "declare -a " << name << "=(";
          for (auto& s : *arr)
            out << escape_shell_arg_always(s) << " ";
          out << ")\n";
        } else if (auto arr = std::get_if<Associative>(&value)) {
          out << "declare -A " << name << "=(";
          for (auto& [n, v] : *arr)
            out << "[" << escape_shell_arg_always(n) << "]=" << escape_shell_arg_always(v) << " ";
          out << ")\n";
        }
      }
    }

    for (auto& [name, def] : bash_functions) {
      out << name << " ()\n{\n" << def << "}\n";
    }
  }

  static std::string get_string(const Value& value) {
    if (auto str = std::get_if<String>(&value))
      return str->value;
    else
      throw Error("bash variable is not a string");
  }

  static Associative get_associative(const Value& value) {
    if (auto assoc = std::get_if<Associative>(&value))
      return *assoc;
    else
      throw Error("bash variable is not an associative array");
  }

  static Array get_strings(const Value& value) {
    if (auto str = std::get_if<String>(&value))
      return tokenize_string<Array>(str->value);
    else if (auto arr = std::get_if<Array>(&value)) {
      return *arr;
    } else if (auto assoc = std::get_if<Associative>(&value)) {
      Array assoc_keys;
      std::for_each(assoc->begin(), assoc->end(), [&](auto& n) { assoc_keys.push_back(n.first); });
      return assoc_keys;
    } else
      throw Error("bash variable is not a string or array");
  }

  bool operator==(const build_environment_t& other) const {
    return vars == other.vars && bash_functions == other.bash_functions;
  }

  std::string get_system() const {
    if (auto v = get(vars, "system"))
      return get_string(*v);
    else
      return settings.thisSystem;
  }
};

const static std::string get_env_sh =
#include "get-env.sh.gen.h"
    ;

/**
 * Given an existing derivation, return the shell environment as
 * initialised by stdenv's setup script. We do this by building a
 * modified derivation with the same dependencies and nearly the same
 * initial environment variables, that just writes the resulting
 * environment to a file and exits.
 */
static StorePath get_derivation_environment(ref<Store> store, ref<Store> eval_store,
                                          const StorePath& drv_path) {
  auto drv = eval_store->derivationFromPath(drv_path);

  auto builder = base_name_of(drv.builder);
  if (builder != "bash")
    throw Error("'nix develop' only works on derivations that use 'bash' as their builder");

  auto get_env_sh_path = ({
    string_source_t source{get_env_sh};
    eval_store->add_to_store_from_dump(source, "get-env.sh", file_serialisation_method_t::flat,
                                  ContentAddressMethod::raw_t::Text, hash_algorithm_t::SHA256, {});
  });

  drv.args = {store->printStorePath(get_env_sh_path)};

  /* Remove derivation checks. */
  if (drv.structured_attrs) {
    drv.structured_attrs->structured_attrs.erase("outputChecks");
  } else {
    drv.env.erase("allowedReferences");
    drv.env.erase("allowedRequisites");
    drv.env.erase("disallowedReferences");
    drv.env.erase("disallowedRequisites");
  }

  drv.env.erase("name");

  /* Rehash and write the derivation. FIXME: would be nice to use
     'buildDerivation', but that's privileged. */
  drv.name += "-env";
  drv.env.emplace("name", drv.name);
  drv.input_srcs.insert(std::move(get_env_sh_path));
  for (auto& [output_name, output] : drv.outputs) {
    std::visit(overloaded{
                   [&](const DerivationOutput::InputAddressed&) {
                     output = DerivationOutput::Deferred{};
                     drv.env[output_name] = "";
                   },
                   [&](const DerivationOutput::CAFixed&) {
                     output = DerivationOutput::Deferred{};
                     drv.env[output_name] = "";
                   },
                   [&](const auto&) {
                     // Do nothing for other types (CAFloating, Deferred, Impure)
                   },
               },
               output.raw);
  }
  drv.fillInOutputPaths(*eval_store);

  auto shell_drv_path = write_derivation(*eval_store, drv);

  /* Build the derivation. */
  store->build_paths({DerivedPath::Built{
                        .drv_path = makeConstantStorePathRef(shell_drv_path),
                        .outputs = OutputsSpec::All{},
                    }},
                    bmNormal, eval_store);

  // `get-env.sh` will write its JSON output to an arbitrary output
  // path, so return the first non-empty output path.
  for (auto& [_0, optPath] : eval_store->queryPartialDerivationOutputMap(shell_drv_path)) {
    assert(optPath);
    auto accessor = eval_store->requireStoreObjectAccessor(*optPath);
    if (auto st = accessor->maybe_lstat(canon_path_t::root); st && st->file_size.value_or(0))
      return *optPath;
  }

  throw Error("get-env.sh failed to produce an environment");
}

struct common_t : InstallableCommand, MixProfile {
  string_set_t ignore_vars{
      "BASHOPTS",
      "HOME", // FIXME: don't ignore in pure mode?
      "NIX_BUILD_TOP", "NIX_ENFORCE_PURITY",
      "NIX_LOG_FD",    "NIX_REMOTE",
      "PPID",          "SHELLOPTS",
      "SSL_CERT_FILE", // FIXME: only want to ignore /no-cert-file.crt
      "TEMP",          "TEMPDIR",
      "TERM",          "TMP",
      "TMPDIR",        "TZ",
      "UID",
  };

  std::vector<std::pair<std::string, std::string>> redirects;

  common_t() {
    add_flag({
        .long_name = "redirect",
        .description = "Redirect a store path to a mutable location.",
        .labels = {"installable", "outputs-dir"},
        .handler = {[&](std::string installable, std::string outputs_dir) {
          redirects.push_back({installable, outputs_dir});
        }},
    });
  }

  std::string make_rc_script(ref<Store> store, const build_environment_t& build_environment,
                           const std::filesystem::path& tmp_dir,
                           const std::filesystem::path& outputs_dir =
                               std::filesystem::path{std::filesystem::current_path()} / "outputs") {
    // A list of colon-separated environment variables that should be
    // prepended to, rather than overwritten, in order to keep the shell usable.
    // Please keep this list minimal in order to avoid impurities.
    static const char* const saved_vars[] = {
        "PATH",          // for commands
        "XDG_DATA_DIRS", // for loadable completion
    };

    std::ostringstream out;

    out << "unset shellHook\n";

    for (auto& var : saved_vars) {
      out << fmt("%s=${%s:-}\n", var, var);
      out << fmt("nix_saved_%s=\"$%s\"\n", var, var);
    }

    build_environment.to_bash(out, ignore_vars);

    for (auto& var : saved_vars)
      out << fmt("%s=\"$%s${nix_saved_%s:+:$nix_saved_%s}\"\n", var, var, var, var);

    out << "export NIX_BUILD_TOP=\"$(mktemp -d -t nix-shell.XXXXXX)\"\n";
    for (auto& i : {"TMP", "TMPDIR", "TEMP", "TEMPDIR"})
      out << fmt("export %s=\"$NIX_BUILD_TOP\"\n", i);

    out << "eval \"${shellHook:-}\"\n";

    auto script = out.str();

    /* Substitute occurrences of output paths. */
    auto outputs = build_environment.vars.find("outputs");
    assert(outputs != build_environment.vars.end());

    string_map_t rewrites;
    if (build_environment.provides_structured_attrs()) {
      for (auto& [output_name, from] : build_environment_t::get_associative(outputs->second)) {
        rewrites.insert({from, (outputs_dir / output_name).string()});
      }
    } else {
      for (auto& output_name : build_environment_t::get_strings(outputs->second)) {
        auto from = build_environment.vars.find(output_name);
        assert(from != build_environment.vars.end());
        rewrites.insert({
            build_environment_t::get_string(from->second),
            (outputs_dir / output_name).string(),
        });
      }
    }

    /* Substitute redirects. */
    for (auto& [installable_, dir_] : redirects) {
      auto dir = abs_path(dir_);
      auto installable = parseInstallable(store, installable_);
      auto built_paths = Installable::toStorePathSet(getEvalStore(), store, Realise::Nothing,
                                                    OperateOn::Output, {installable});
      for (auto& path : built_paths) {
        auto from = store->printStorePath(path);
        if (script.find(from) == std::string::npos)
          warn("'%s' (path '%s') is not used by this build environment", installable->what(), from);
        else {
          printInfo("redirecting '%s' to '%s'", from, dir);
          rewrites.insert({from, dir});
        }
      }
    }

    if (build_environment.provides_structured_attrs()) {
      fixup_structured_attrs(OS_STR("sh"), "NIX_ATTRS_SH_FILE", build_environment.get_attrs_sh(),
                           rewrites, build_environment, tmp_dir);
      fixup_structured_attrs(OS_STR("json"), "NIX_ATTRS_JSON_FILE", build_environment.get_attrs_json(),
                           rewrites, build_environment, tmp_dir);
    }

    return rewrite_strings(script, rewrites);
  }

  /**
   * Replace the value of NIX_ATTRS_*_FILE (`/build/.attrs.*`) with a tmp file
   * that's accessible from the interactive shell session.
   */
  void fixup_structured_attrs(path_view_ng_t::string_view ext, const std::string& env_var,
                            const std::string& content, string_map_t& rewrites,
                            const build_environment_t& build_environment,
                            const std::filesystem::path& tmp_dir) {
    auto target_file_path = tmp_dir / OS_STR(".attrs.");
    target_file_path += ext;

    write_file(target_file_path, content);

    auto file_in_builder_env = build_environment.vars.find(env_var);
    assert(file_in_builder_env != build_environment.vars.end());
    rewrites.insert(
        {build_environment_t::get_string(file_in_builder_env->second), target_file_path.string()});
  }

  strings_t getDefaultFlakeAttrPaths() override {
    strings_t paths{
        "devShells." + settings.thisSystem.get() + ".default",
        "devShell." + settings.thisSystem.get(),
    };
    for (auto& p : SourceExprCommand::getDefaultFlakeAttrPaths())
      paths.push_back(p);
    return paths;
  }

  strings_t getDefaultFlakeAttrPathPrefixes() override {
    auto res = SourceExprCommand::getDefaultFlakeAttrPathPrefixes();
    res.emplace_front("devShells." + settings.thisSystem.get() + ".");
    return res;
  }

  StorePath get_shell_out_path(ref<Store> store, ref<Installable> installable) {
    auto path = installable->getStorePath();
    if (path && has_suffix(path->to_string(), "-env"))
      return *path;
    else {
      auto drvs = Installable::toDerivations(store, {installable});

      if (drvs.size() != 1)
        throw Error(
            "'%s' needs to evaluate to a single derivation, but it evaluated to %d derivations",
            installable->what(), drvs.size());

      auto& drv_path = *drvs.begin();

      return get_derivation_environment(store, getEvalStore(), drv_path);
    }
  }

  std::pair<build_environment_t, StorePath> get_build_environment(ref<Store> store,
                                                             ref<Installable> installable) {
    auto shell_out_path = get_shell_out_path(store, installable);

    updateProfile(shell_out_path);

    debug("reading environment file '%s'", store->printStorePath(shell_out_path));

    return {
        build_environment_t::parse_json(
            store->requireStoreObjectAccessor(shell_out_path)->read_file(canon_path_t::root)),
        shell_out_path,
    };
  }
};

struct cmd_develop_t : common_t, MixEnvironment {
  std::vector<std::string> command;
  std::optional<std::string> phase;

  cmd_develop_t() {
    add_flag({
        .long_name = "command",
        .short_name = 'c',
        .description =
            "Instead of starting an interactive shell, start the specified command and arguments.",
        .labels = {"command", "args"},
        .handler = {[&](std::vector<std::string> ss) {
          if (ss.empty())
            throw UsageError("--command requires at least one argument");
          command = ss;
        }},
    });

    add_flag({
        .long_name = "phase",
        .description = "The stdenv phase to run (e.g. `build` or `configure`).",
        .labels = {"phase-name"},
        .handler = {&phase},
    });

    add_flag({
        .long_name = "unpack",
        .description = "Run the `unpack` phase.",
        .handler = {&phase, {"unpack"}},
    });

    add_flag({
        .long_name = "configure",
        .description = "Run the `configure` phase.",
        .handler = {&phase, {"configure"}},
    });

    add_flag({
        .long_name = "build",
        .description = "Run the `build` phase.",
        .handler = {&phase, {"build"}},
    });

    add_flag({
        .long_name = "check",
        .description = "Run the `check` phase.",
        .handler = {&phase, {"check"}},
    });

    add_flag({
        .long_name = "install",
        .description = "Run the `install` phase.",
        .handler = {&phase, {"install"}},
    });

    add_flag({
        .long_name = "installcheck",
        .description = "Run the `installcheck` phase.",
        .handler = {&phase, {"installCheck"}},
    });
  }

  std::string description() override {
    return "run a bash shell that provides the build environment of a derivation";
  }

  std::string doc() override {
    return
#include "develop.md"
        ;
  }

  void run(ref<Store> store, ref<Installable> installable) override {
    auto [build_environment, gcroot] = get_build_environment(store, installable);

    auto [rcFileFd, rcFilePath] = create_temp_file("nix-shell");

    auto_delete_t tmp_dir(create_temp_dir("", "nix-develop"), true);

    auto script = make_rc_script(store, build_environment, tmp_dir);

    if (verbosity >= lvl_debug)
      script += "set -x\n";

    script += fmt("command rm -f '%s'\n", rcFilePath);

    if (phase) {
      if (!command.empty())
        throw UsageError("you cannot use both '--command' and '--phase'");
      // FIXME: foundMakefile is set by buildPhase, need to get
      // rid of that.
      script += fmt("foundMakefile=1\n");
      script += fmt("runHook %1%Phase\n", *phase);
    }

    else if (!command.empty()) {
      std::vector<std::string> args;
      args.reserve(command.size());
      for (const auto& s : command)
        args.push_back(escape_shell_arg_always(s));
      script += fmt("exec %s\n", concat_strings_sep(" ", args));
    }

    else {
      script =
          "[ -n \"$PS1\" ] && [ -e ~/.bashrc ] && source ~/.bashrc;\nshopt -u expand_aliases\n" +
          script + "\nshopt -s expand_aliases\n";
      if (develop_settings.bash_prompt != "")
        script += fmt("[ -n \"$PS1\" ] && PS1=%s;\n",
                      escape_shell_arg_always(develop_settings.bash_prompt.get()));
      if (develop_settings.bash_prompt_prefix != "")
        script += fmt("[ -n \"$PS1\" ] && PS1=%s\"$PS1\";\n",
                      escape_shell_arg_always(develop_settings.bash_prompt_prefix.get()));
      if (develop_settings.bash_prompt_suffix != "")
        script += fmt("[ -n \"$PS1\" ] && PS1+=%s;\n",
                      escape_shell_arg_always(develop_settings.bash_prompt_suffix.get()));
    }

    setEnviron();
    // prevent garbage collection until shell exits
    set_env("NIX_GCROOT", store->printStorePath(gcroot).c_str());

    Path shell = "bash";
    bool found_interactive = false;

    try {
      auto state = getEvalState();

      auto nixpkgs_lock_flags = lock_flags;
      nixpkgs_lock_flags.inputOverrides = {};
      nixpkgs_lock_flags.inputUpdates = {};

      auto nixpkgs = defaultNixpkgsFlakeRef();
      if (auto* i = dynamic_cast<const InstallableFlake*>(&*installable))
        nixpkgs = i->nixpkgsFlakeRef();

      auto bash_installable = make_ref<InstallableFlake>(
          nullptr, //< Don't barf when the command is run with --arg/--argstr
          state, std::move(nixpkgs), "bashInteractive", ExtendedOutputsSpec::Default(), strings_t{},
          strings_t{"legacyPackages." + settings.thisSystem.get() + "."}, nixpkgs_lock_flags);

      for (auto& path : Installable::toStorePathSet(getEvalStore(), store, Realise::Outputs,
                                                    OperateOn::Output, {bash_installable})) {
        auto s = store->printStorePath(path) + "/bin/bash";
        if (path_exists(s)) {
          shell = s;
          found_interactive = true;
          break;
        }
      }

      if (!found_interactive)
        throw Error("package 'nixpkgs#bashInteractive' does not provide a 'bin/bash'");

    } catch (Error&) {
      ignore_exception_except_interrupt();
    }

    // Override SHELL with the one chosen for this environment.
    // This is to make sure the system shell doesn't leak into the build environment.
    set_env("SHELL", shell.c_str());
    // https://github.com/NixOS/nix/issues/5873
    script += fmt("SHELL=\"%s\"\n", shell);
    if (found_interactive)
      script += fmt("PATH=\"%s${PATH:+:$PATH}\"\n", std::filesystem::path(shell).parent_path());
    write_full(rcFileFd.get(), script);

#ifdef _WIN32 // TODO re-enable on Windows
    throw UnimplementedError("Cannot yet spawn processes on Windows");
#else
    // If running a phase or single command, don't want an interactive shell running after
    // Ctrl-C, so don't pass --rcfile
    auto args = phase || !command.empty()
                    ? strings_t{std::string(base_name_of(shell)), rcFilePath}
                    : strings_t{std::string(base_name_of(shell)), "--rcfile", rcFilePath};

    // Need to chdir since phases assume in flake directory
    if (phase) {
      // chdir if installable is a flake of type git+file or path
      auto installable_flake = installable.dynamic_pointer_cast<InstallableFlake>();
      if (installable_flake) {
        auto source_path =
            installable_flake->getLockedFlake()->flake.resolved_ref.input.get_source_path();
        if (source_path) {
          if (chdir(source_path->c_str()) == -1) {
            throw sys_error_t("chdir to %s failed", *source_path);
          }
        }
      }
    }

    // Release our references to eval caches to ensure they are persisted to disk, because
    // we are about to exec out of this process without running C++ destructors.
    getEvalState()->evalCaches.clear();

    exec_program_in_store(store, use_lookup_path_t::use, shell, args, build_environment.get_system());
#endif
  }
};

struct cmd_print_dev_env_t : common_t, MixJSON {
  std::string description() override {
    return "print shell code that can be sourced by bash to reproduce the build environment of a "
           "derivation";
  }

  std::string doc() override {
    return
#include "print-dev-env.md"
        ;
  }

  category_t category() override { return catUtility; }

  void run(ref<Store> store, ref<Installable> installable) override {
    auto build_environment = get_build_environment(store, installable).first;

    logger->stop();

    if (json) {
      printJSON(build_environment.to_json());
    } else {
      auto_delete_t tmp_dir(create_temp_dir("", "nix-dev-env"), true);
      logger->write_to_stdout(make_rc_script(store, build_environment, tmp_dir));
    }
  }
};

static auto r_cmd_print_dev_env = registerCommand<cmd_print_dev_env_t>("print-dev-env");
static auto r_cmd_develop = registerCommand<cmd_develop_t>("develop");
