#pragma once
///@file

#include <optional>

#include "nix/cmd/common-eval-args.h"
#include "nix/cmd/installable-value.h"
#include "nix/flake/lockfile.h"
#include "nix/store/path.h"
#include "nix/util/args.h"

namespace nix {

extern std::string program_path;

extern char** saved_argv;

class eval_state_t;
struct pos_t;
class store_t;
struct local_fs_store;

static constexpr command_t::category_t catHelp = -1;
static constexpr command_t::category_t catSecondary = 100;
static constexpr command_t::category_t catUtility = 101;
static constexpr command_t::category_t catNixInstallation = 102;

static constexpr auto installablesCategory =
    "Options that change the interpretation of "
    "[installables](@docroot@/command-ref/new-cli/nix.md#installables)";

struct NixMultiCommand : multi_command_t, virtual command_t {
  nlohmann::json to_json() override;

  using multi_command_t::multi_command_t;

  virtual void run() override;
};

// For the overloaded run methods
#pragma GCC diagnostic ignored "-Woverloaded-virtual"

/**
 * A command that requires a \ref store_t "Nix store".
 */
struct StoreCommand : virtual command_t {
  StoreCommand();
  void run() override;

  /**
   * Return the default Nix store.
   */
  ref<store_t> getStore();

  /**
   * Return the destination Nix store.
   */
  virtual ref<store_t> getDstStore() { return getStore(); }

  virtual ref<store_t> createStore();
  /**
   * Main entry point, with a `store_t` provided
   */
  virtual void run(ref<store_t>) = 0;

private:
  std::shared_ptr<store_t> _store;
};

/**
 * A command that copies something between `--from` and `--to` \ref
 * store_t stores.
 */
struct CopyCommand : virtual StoreCommand {
  std::string srcUri, dst_uri;

  CopyCommand();

  ref<store_t> createStore() override;

  ref<store_t> getDstStore() override;
};

/**
 * A command that needs to evaluate Nix language expressions.
 */
struct EvalCommand : virtual StoreCommand, MixEvalArgs {
  bool startReplOnEvalErrors = false;
  bool ignoreExceptionsDuringTry = false;

  EvalCommand();

  ~EvalCommand();

  ref<store_t> getEvalStore();

  ref<eval_state_t> getEvalState();

private:
  std::shared_ptr<store_t> eval_store;

  std::shared_ptr<eval_state_t> eval_state;
};

/**
 * A mixin class for commands that process flakes, adding a few standard
 * flake-related options/flags.
 */
struct MixFlakeOptions : virtual args_t, EvalCommand {
  flake::LockFlags lock_flags;

  MixFlakeOptions();

  /**
   * The completion for some of these flags depends on the flake(s) in
   * question.
   *
   * This method should be implemented to gather all flakerefs the
   * command is operating with (presumably specified via some other
   * arguments) so that the completions for these flags can use them.
   */
  virtual std::vector<flake_ref_t> get_flake_refs_for_completion() { return {}; }
};

struct SourceExprCommand : virtual args_t, MixFlakeOptions {
  std::optional<std::filesystem::path> file;
  std::optional<std::string> expr;

  SourceExprCommand();

  Installables parseInstallables(ref<store_t> store, std::vector<std::string> ss);

  ref<Installable> parseInstallable(ref<store_t> store, const std::string& installable);

  virtual strings_t getDefaultFlakeAttrPaths();

  virtual strings_t getDefaultFlakeAttrPathPrefixes();

  /**
   * Complete an installable from the given prefix.
   */
  void completeInstallable(add_completions_t& completions, std::string_view prefix);

  /**
   * Convenience wrapper around the underlying function to make setting the
   * callback easier.
   */
  completer_closure_t getCompleteInstallable();
};

/**
 * A mixin class for commands that need a read-only flag.
 *
 * What exactly is "read-only" is unspecified, but it will usually be
 * the \ref store_t "Nix store".
 */
struct MixReadOnlyOption : virtual args_t {
  MixReadOnlyOption();
};

/**
 * Like InstallablesCommand but the installables are not loaded.
 *
 * This is needed by `cmd_repl_t` which wants to load (and reload) the
 * installables itself.
 */
struct RawInstallablesCommand : virtual args_t, SourceExprCommand {
  RawInstallablesCommand();

  virtual void run(ref<store_t> store, std::vector<std::string>&& raw_installables) = 0;

  void run(ref<store_t> store) override;

  // FIXME make const after `CmdRepl`'s override is fixed up
  virtual void applyDefaultInstallables(std::vector<std::string>& raw_installables);

  bool readFromStdIn = false;

  std::vector<flake_ref_t> get_flake_refs_for_completion() override;

private:
  std::vector<std::string> raw_installables;
};

/**
 * A command that operates on a list of "installables", which can be
 * store paths, attribute paths, Nix expressions, etc.
 */
struct InstallablesCommand : RawInstallablesCommand {
  virtual void run(ref<store_t> store, Installables&& installables) = 0;

  void run(ref<store_t> store, std::vector<std::string>&& raw_installables) override;
};

/**
 * A command that operates on exactly one "installable".
 */
struct InstallableCommand : virtual args_t, SourceExprCommand {
  InstallableCommand();

  virtual void preRun(ref<store_t> store);

  virtual void run(ref<store_t> store, ref<Installable> installable) = 0;

  void run(ref<store_t> store) override;

  std::vector<flake_ref_t> get_flake_refs_for_completion() override;

private:
  std::string _installable{"."};
};

struct MixOperateOnOptions : virtual args_t {
  OperateOn operateOn = OperateOn::Output;

  MixOperateOnOptions();
};

/**
 * A command that operates on zero or more extant store paths.
 *
 * If the argument the user passes is a some sort of recipe for a path
 * not yet built, it must be built first.
 */
struct BuiltPathsCommand : InstallablesCommand, virtual MixOperateOnOptions {
private:
  bool recursive = false;
  bool all = false;

protected:
  Realise realiseMode = Realise::derivation_t;

public:
  BuiltPathsCommand(bool recursive = false);

  virtual void run(ref<store_t> store, BuiltPaths&& all_paths, BuiltPaths&& root_paths) = 0;

  void run(ref<store_t> store, Installables&& installables) override;

  void applyDefaultInstallables(std::vector<std::string>& raw_installables) override;
};

struct StorePathsCommand : public BuiltPathsCommand {
  StorePathsCommand(bool recursive = false);

  virtual void run(ref<store_t> store, store_paths_t&& store_paths) = 0;

  void run(ref<store_t> store, BuiltPaths&& all_paths, BuiltPaths&& root_paths) override;
};

/**
 * A command that operates on exactly one store path.
 */
struct StorePathCommand : public StorePathsCommand {
  virtual void run(ref<store_t> store, const store_path_t& store_path) = 0;

  void run(ref<store_t> store, store_paths_t&& store_paths) override;
};

/**
 * A helper class for registering \ref command_t commands globally.
 */
struct RegisterCommand {
  typedef std::map<std::vector<std::string>, std::function<ref<command_t>()>> commands_t;

  static commands_t& commands();

  RegisterCommand(std::vector<std::string>&& name, std::function<ref<command_t>()> command) {
    commands().emplace(name, command);
  }

  static nix::commands_t getCommandsFor(const std::vector<std::string>& prefix);
};

template <class T>
static RegisterCommand registerCommand(const std::string& name) {
  return RegisterCommand({name}, []() { return make_ref<T>(); });
}

template <class T>
static RegisterCommand registerCommand2(std::vector<std::string>&& name) {
  return RegisterCommand(std::move(name), []() { return make_ref<T>(); });
}

struct MixProfile : virtual StoreCommand {
  std::optional<std::filesystem::path> profile;

  MixProfile();

  /* If 'profile' is set, make it point at 'storePath'. */
  void updateProfile(const store_path_t& store_path);

  /* If 'profile' is set, make it point at the store path produced
     by 'buildables'. */
  void updateProfile(const BuiltPaths& buildables);
};

struct MixDefaultProfile : MixProfile {
  MixDefaultProfile();
};

struct MixEnvironment : virtual args_t {
  string_set_t keepVars;
  string_set_t unsetVars;
  string_map_t setVars;
  bool ignoreEnvironment;

  MixEnvironment();

  /***
   * Modify global environ based on `ignoreEnvironment`, `keep`,
   * `unset`, and `added`. It's expected that exec will be called
   * before this class goes out of scope, otherwise `environ` will
   * become invalid.
   */
  void setEnviron();
};

struct MixNoCheckSigs : virtual args_t {
  CheckSigsFlag check_sigs = CheckSigs;

  MixNoCheckSigs() {
    add_flag({
        .long_name = "no-check-sigs",
        .description = "Do not require that paths are signed by trusted keys.",
        .handler = {&check_sigs, NoCheckSigs},
    });
  }
};

void complete_flake_input_attr_path(add_completions_t& completions, ref<eval_state_t> eval_state,
                                const std::vector<flake_ref_t>& flake_refs, std::string_view prefix);

void complete_flake_ref(add_completions_t& completions, ref<store_t> store, std::string_view prefix);

void complete_flake_ref_with_fragment(add_completions_t& completions, ref<eval_state_t> eval_state,
                                  flake::LockFlags lock_flags, strings_t attr_path_prefixes,
                                  const strings_t& default_flake_attr_paths, std::string_view prefix);

std::string show_versions(const string_set_t& versions);

void print_closure_diff(ref<store_t> store, const store_path_t& before_path, const store_path_t& after_path,
                      std::string_view indent);

/**
 * Create symlinks prefixed by `out_link` to the store paths in
 * `buildables`.
 */
void create_out_links(const std::filesystem::path& out_link, const BuiltPaths& buildables,
                    local_fs_store& store);

/** `out_link` parameter, `createOutLinksMaybe` method. See `MixOutLinkByDefault`. */
struct MixOutLinkBase : virtual args_t {
  /** Prefix for any output symlinks. Empty means do not write an output symlink. */
  Path out_link;

  MixOutLinkBase(const std::string& defaultOutLink) : out_link(defaultOutLink) {}

  void createOutLinksMaybe(const std::vector<BuiltPathWithResult>& buildables, ref<store_t>& store);
};

/** `--out-link`, `--no-link`, `createOutLinksMaybe` */
struct MixOutLinkByDefault : MixOutLinkBase, virtual args_t {
  MixOutLinkByDefault() : MixOutLinkBase("result") {
    add_flag({
        .long_name = "out-link",
        .short_name = 'o',
        .description =
            "Use *path* as prefix for the symlinks to the build results. It defaults to `result`.",
        .labels = {"path"},
        .handler = {&out_link},
        .completer = complete_path,
    });

    add_flag({
        .long_name = "no-link",
        .description = "Do not create symlinks to the build results.",
        .handler = {&out_link, Path("")},
    });
  }
};

} // namespace nix
