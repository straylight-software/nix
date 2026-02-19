#include <regex>

#include <sys/types.h>

#include <nlohmann/json.hpp>

#include "cli-config-private.h"
#include "crash-handler.h"
#include "nix/cmd/command.h"
#include "nix/cmd/legacy.h"
#include "nix/cmd/markdown.h"
#include "nix/cmd/network-proxy.h"
#include "nix/expr/eval-cache.h"
#include "nix/expr/eval-settings.h"
#include "nix/expr/eval.h"
#include "nix/flake/flake.h"
#include "nix/flake/settings.h"
#include "nix/main/common-args.h"
#include "nix/main/loggers.h"
#include "nix/main/shared.h"
#include "nix/store/filetransfer.h"
#include "nix/store/globals.h"
#include "nix/store/store-open.h"
#include "nix/store/store-registration.h"
#include "nix/util/args/root.h"
#include "nix/util/current-process.h"
#include "nix/util/finally.h"
#include "nix/util/json-utils.h"
#include "nix/util/memory-source-accessor.h"
#include "nix/util/terminal.h"
#include "nix/util/users.h"
#include "self-exe.h"

#ifndef _WIN32
#  include <ifaddrs.h>
#  include <netdb.h>
#  include <netinet/in.h>
#  include <sys/socket.h>
#endif

#ifdef __linux__
#  include "nix/util/linux-namespaces.h"
#endif

#ifndef _WIN32
extern std::string chroot_helper_name;

void chroot_helper(int argc, char** argv);
#endif

#include "nix/util/strings.h"

namespace nix {

/* Check if we have a non-loopback/link-local network interface. */
static bool have_internet() {
#ifndef _WIN32
  struct ifaddrs* addrs;

  if (getifaddrs(&addrs))
    return true;

  finally_t free([&]() { freeifaddrs(addrs); });

  for (auto i = addrs; i; i = i->ifa_next) {
    if (!i->ifa_addr)
      continue;
    if (i->ifa_addr->sa_family == AF_INET) {
      if (ntohl(((sockaddr_in*)i->ifa_addr)->sin_addr.s_addr) != INADDR_LOOPBACK) {
        return true;
      }
    } else if (i->ifa_addr->sa_family == AF_INET6) {
      if (!IN6_IS_ADDR_LOOPBACK(&((sockaddr_in6*)i->ifa_addr)->sin6_addr) &&
          !IN6_IS_ADDR_LINKLOCAL(&((sockaddr_in6*)i->ifa_addr)->sin6_addr))
        return true;
    }
  }

  if (have_network_proxy_connection())
    return true;

  return false;
#else
  // TODO implement on Windows
  return true;
#endif
}

static void disable_net() {
  // FIXME: should check for command line overrides only.
  if (!settings.use_substitutes.overridden)
    // FIXME: should not disable local substituters (like file:///).
    settings.use_substitutes = false;
  if (!settings.tarballTtl.overridden)
    settings.tarballTtl = std::numeric_limits<unsigned int>::max();
  if (!file_transfer_settings.tries.overridden)
    file_transfer_settings.tries = 0;
  if (!file_transfer_settings.connectTimeout.overridden)
    file_transfer_settings.connectTimeout = 1;
}

std::string program_path;

struct nix_args_t : virtual multi_command_t, virtual MixCommonArgs, virtual root_args_t {
  bool use_net = true;
  bool refresh = false;
  bool help_requested = false;
  bool show_version = false;

  nix_args_t() : multi_command_t("", RegisterCommand::getCommandsFor({})), MixCommonArgs("nix") {
    get_categories().clear();
    get_categories()[catHelp] = "Help commands";
    get_categories()[command_t::cat_default] = "Main commands";
    get_categories()[catSecondary] = "Infrequently used commands";
    get_categories()[catUtility] = "Utility/scripting commands";
    get_categories()[catNixInstallation] =
        "Commands for upgrading or troubleshooting your Nix installation";

    add_flag({
        .long_name = "help",
        .description = "Show usage information.",
        .category = miscCategory,
        .handler = {[this]() { this->help_requested = true; }},
    });

    add_flag({
        .long_name = "print-build-logs",
        .short_name = 'L',
        .description = "Print full build logs on standard error.",
        .category = loggingCategory,
        .handler = {[&]() { logger->set_print_build_logs(true); }},
    });

    add_flag({
        .long_name = "version",
        .description = "Show version information.",
        .category = miscCategory,
        .handler = {[&]() { show_version = true; }},
    });

    add_flag({
        .long_name = "offline",
        .aliases = {"no-net"}, // FIXME: remove
        .description =
            "Disable substituters and consider all previously downloaded files up-to-date.",
        .category = miscCategory,
        .handler = {[&]() { use_net = false; }},
    });

    add_flag({
        .long_name = "refresh",
        .description = "Consider all previously downloaded files out-of-date.",
        .category = miscCategory,
        .handler = {[&]() { refresh = true; }},
    });

    get_aliases() = {
        {"add-to-store", {alias_status_t::deprecated, {"store", "add-path"}}},
        {"cat-nar", {alias_status_t::deprecated, {"nar", "cat"}}},
        {"cat-store", {alias_status_t::deprecated, {"store", "cat"}}},
        {"copy-sigs", {alias_status_t::deprecated, {"store", "copy-sigs"}}},
        {"dev-shell", {alias_status_t::deprecated, {"develop"}}},
        {"diff-closures", {alias_status_t::deprecated, {"store", "diff-closures"}}},
        {"dump-path", {alias_status_t::deprecated, {"store", "dump-path"}}},
        {"hash-file", {alias_status_t::deprecated, {"hash", "file"}}},
        {"hash-path", {alias_status_t::deprecated, {"hash", "path"}}},
        {"ls-nar", {alias_status_t::deprecated, {"nar", "ls"}}},
        {"ls-store", {alias_status_t::deprecated, {"store", "ls"}}},
        {"make-content-addressable",
         {alias_status_t::deprecated, {"store", "make-content-addressed"}}},
        {"optimise-store", {alias_status_t::deprecated, {"store", "optimise"}}},
        {"ping-store", {alias_status_t::deprecated, {"store", "info"}}},
        {"sign-paths", {alias_status_t::deprecated, {"store", "sign"}}},
        {"shell", {alias_status_t::accepted_shorthand, {"env", "shell"}}},
        {"show-derivation", {alias_status_t::deprecated, {"derivation", "show"}}},
        {"show-config", {alias_status_t::deprecated, {"config", "show"}}},
        {"to-base16", {alias_status_t::deprecated, {"hash", "to-base16"}}},
        {"to-base32", {alias_status_t::deprecated, {"hash", "to-base32"}}},
        {"to-base64", {alias_status_t::deprecated, {"hash", "to-base64"}}},
        {"verify", {alias_status_t::deprecated, {"store", "verify"}}},
        {"doctor", {alias_status_t::deprecated, {"config", "check"}}},
    };
  }

  std::string description() override {
    return "a tool for reproducible and declarative configuration management";
  }

  std::string doc() override {
    return
#include "nix.md"
        ;
  }

  // Plugins may add new subcommands.
  void plugins_inited() override { get_commands() = RegisterCommand::getCommandsFor({}); }

  std::string dump_cli() {
    using nlohmann::json;

    auto res = json::object();

    res["args"] = to_json();

    {
      auto& stores = res["stores"] = json::object();
      for (auto& [storeName, implem] : Implementations::registered()) {
        auto& j = stores[storeName];
        j["doc"] = implem.doc;
        j["uri-schemes"] = implem.uriSchemes;
        j["settings"] = implem.getConfig()->to_json();
        j["experimentalFeature"] = implem.experimental_feature;
      }
    }

    {
      auto& fetchers = res["fetchers"] = json::object();

      for (const auto& [schemeName, scheme] : fetchers::get_all_input_schemes()) {
        auto& s = fetchers[schemeName] = json::object();
        s["description"] = scheme->schemeDescription();
        auto& attrs = s["allowedAttrs"] = json::object();
        for (auto& [fieldName, field] : scheme->allowed_attrs()) {
          auto& f = attrs[fieldName] = json::object();
          f["type"] = field.type;
          f["required"] = field.required;
          f["doc"] = strip_indentation(field.doc);
        }
      }
    };

    return res.dump();
  }
};

/* Render the help for the specified subcommand to stdout using
   lowdown. */
static void show_help(std::vector<std::string> subcommand, nix_args_t& toplevel) {
  // Check for aliases if subcommand has exactly one element
  if (subcommand.size() == 1) {
    auto alias = toplevel.get_aliases().find(subcommand[0]);
    if (alias != toplevel.get_aliases().end()) {
      subcommand = alias->second.replacement;
    }
  }

  auto mdName = subcommand.empty() ? "nix" : fmt("nix3-%s", concat_strings_sep("-", subcommand));

  eval_settings.restrictEval = true;
  eval_settings.pureEval = true;
  EvalState state({}, open_store("dummy://"), fetch_settings, eval_settings);

  auto vGenerateManpage = state.allocValue();
  state.eval(state.parseExprFromString(
#include "generate-manpage.nix.gen.h"
                 , state.root_path(canon_path_t::root)),
             *vGenerateManpage);

  state.corepkgsFS->add_file(canon_path_t("utils.nix"),
#include "utils.nix.gen.h"
  );

  state.corepkgsFS->add_file(canon_path_t("/generate-settings.nix"),
#include "generate-settings.nix.gen.h"
  );

  state.corepkgsFS->add_file(canon_path_t("/generate-store-info.nix"),
#include "generate-store-info.nix.gen.h"
  );

  auto vDump = state.allocValue();
  vDump->mk_string(toplevel.dump_cli(), state.mem);

  auto v_res = state.allocValue();
  Value* args[]{&state.getBuiltin("false"), vDump};
  state.callFunction(*vGenerateManpage, args, *v_res, no_pos);

  auto attr = v_res->attrs()->get(state.symbols.create(mdName + ".md"));
  if (!attr)
    throw UsageError("Nix has no subcommand '%s'", concat_strings_sep("", subcommand));

  auto markdown = state.forceString(*attr->value, no_pos, "while evaluating the lowdown help text");

  RunPager pager;
  std::cout << render_markdown_to_terminal(markdown) << "\n";
}

static nix_args_t& get_nix_args(command_t& cmd) {
  return dynamic_cast<nix_args_t&>(cmd.get_root());
}

struct cmd_help_t : command_t {
  std::vector<std::string> subcommand;

  cmd_help_t() {
    expect_args({
        .label = "subcommand",
        .handler = {&subcommand},
    });
  }

  std::string description() override { return "show help about `nix` or a particular subcommand"; }

  std::string doc() override {
    return
#include "help.md"
        ;
  }

  category_t category() override { return catHelp; }

  void run() override {
    assert(get_parent());
    multi_command_t* toplevel = get_parent();
    while (toplevel->get_parent())
      toplevel = toplevel->get_parent();
    show_help(subcommand, get_nix_args(*this));
  }
};

static auto r_cmd_help = registerCommand<cmd_help_t>("help");

struct cmd_help_stores_t : command_t {
  std::string description() override { return "show help about store types and their settings"; }

  std::string doc() override {
    return
#include "help-stores.md.gen.h"
        ;
  }

  category_t category() override { return catHelp; }

  void run() override { show_help({"help-stores"}, get_nix_args(*this)); }
};

static auto r_cmd_help_stores = registerCommand<cmd_help_stores_t>("help-stores");

void main_wrapped(int argc, char** argv) {
  saved_argv = argv;

  register_crash_handler();

  /* The chroot helper needs to be run before any threads have been
     started. */
#ifndef _WIN32
  if (argc > 0 && argv[0] == chroot_helper_name) {
    chroot_helper(argc, argv);
    return;
  }
#endif

  init_nix();
  init_gc();
  flake_settings.configureEvalSettings(eval_settings);

  /* Set the build hook location

     For builds we perform a self-invocation, so Nix has to be
     self-aware. That is, it has to know where it is installed. We
     don't think it's sentient.
   */
  settings.buildHook.set_default(strings_t{
      get_nix_bin({}).string(),
      "__build-remote",
  });

#ifdef __linux__
  if (is_root_user()) {
    try {
      save_mount_namespace();
      if (unshare(CLONE_NEWNS) == -1)
        throw sys_error_t("setting up a private mount namespace");
    } catch (Error& e) {
    }
  }
#endif

  program_path = argv[0];
  auto program_name = std::string(base_name_of(program_path));
  auto extension_pos = program_name.find_last_of(".");
  if (extension_pos != std::string::npos)
    program_name.erase(extension_pos);

  if (argc > 1 && std::string_view(argv[1]) == "__build-remote") {
    program_name = "build-remote";
    argv++;
    argc--;
  }

  {
    auto legacy = RegisterLegacyCommand::commands()[program_name];
    if (legacy)
      return legacy(argc, argv);
  }

  eval_settings.pureEval = true;

  set_log_format("bar");
  settings.verbose_build = false;

  // If on a terminal, progress will be displayed via progress bars etc. (thus verbosity=notice)
  if (nix::is_tty()) {
    verbosity = lvl_notice;
  } else {
    verbosity = lvl_info;
  }

  nix_args_t args;

  if (argc == 2 && std::string(argv[1]) == "__dump-cli") {
    logger->cout(args.dump_cli());
    return;
  }

  if (argc == 2 && std::string(argv[1]) == "__dump-language") {
    experimental_feature_settings.experimental_features = {
        xp_t::fetch_closure,
        xp_t::dynamic_derivations,
        xp_t::fetch_tree,
    };
    eval_settings.pureEval = false;
    EvalState state({}, open_store("dummy://"), fetch_settings, eval_settings);
    auto builtins_json = nlohmann::json::object();
    for (auto& builtinPtr : state.getBuiltins().attrs()->lexicographicOrder(state.symbols)) {
      auto& builtin = *builtinPtr;
      auto b = nlohmann::json::object();
      if (!builtin.value->isPrimOp())
        continue;
      auto prim_op = builtin.value->prim_op();
      if (!prim_op->doc)
        continue;
      b["args"] = prim_op->args;
      b["doc"] = trim(strip_indentation(*prim_op->doc));
      if (prim_op->experimental_feature)
        b["experimental-feature"] = prim_op->experimental_feature;
      builtins_json.emplace(state.symbols[builtin.name], std::move(b));
    }
    for (auto& [name, info] : state.constantInfos) {
      auto b = nlohmann::json::object();
      if (!info.doc)
        continue;
      b["doc"] = trim(strip_indentation(info.doc));
      b["type"] = show_type(info.type, false);
      if (info.impureOnly)
        b["impure-only"] = true;
      builtins_json[name] = std::move(b);
    }
    logger->cout("%s", builtins_json);
    return;
  }

  if (argc == 2 && std::string(argv[1]) == "__dump-xp-features") {
    logger->cout(document_experimental_features().dump());
    return;
  }

  finally_t print_completions([&]() {
    if (args.completions) {
      switch (args.completions->type) {
        case completions_t::Type::normal:
          logger->cout("normal");
          break;
        case completions_t::Type::filenames:
          logger->cout("filenames");
          break;
        case completions_t::Type::attrs:
          logger->cout("attrs");
          break;
      }
      for (auto& s : args.completions->completions)
        logger->cout(s.get_completion() + "\t" + trim(s.get_description()));
    }
  });

  if (get_env("NIX_GET_COMPLETIONS"))
    /* Avoid fetching stuff during tab completion. We have to this
       early because we haven't checked `have_internet()` yet
       (below). */
    disable_net();

  try {
    auto is_nix_command = std::regex_search(program_name, std::regex("nix$"));
    auto allow_shebang = is_nix_command && argc > 1;
    args.parse_cmdline(argv_to_strings(argc, argv), allow_shebang);
  } catch (UsageError&) {
    if (!args.help_requested && !args.completions)
      throw;
  }

  apply_json_logger();

  printTalkative("Nix %s", version());

  if (args.help_requested) {
    std::vector<std::string> subcommand;
    multi_command_t* command = &args;
    while (command) {
      if (command && command->get_command()) {
        subcommand.push_back(command->get_command()->first);
        command = dynamic_cast<multi_command_t*>(&*command->get_command()->second);
      } else
        break;
    }
    show_help(subcommand, args);
    return;
  }

  if (args.completions)
    return;

  if (args.show_version) {
    print_version(program_name);
    return;
  }

  if (!args.get_command())
    throw UsageError("no subcommand specified");

  experimental_feature_settings.require(args.get_command()->second->experimental_feature());

  if (args.use_net && !have_internet()) {
    warn("you don't have Internet access; disabling some network-dependent features");
    args.use_net = false;
  }

  if (!args.use_net)
    disable_net();

  if (args.refresh) {
    settings.tarballTtl = 0;
    settings.ttlNegativeNarInfoCache = 0;
    settings.ttlPositiveNarInfoCache = 0;
  }

  if (args.get_command()->second->force_impure_by_default() && !eval_settings.pureEval.overridden) {
    eval_settings.pureEval = false;
  }

  try {
    args.get_command()->second->run();
  } catch (eval_cache::CachedEvalError& e) {
    /* Evaluate the original attribute that resulted in this
       cached error so that we can show the original error to the
       user. */
    e.force();
  }
}

} // namespace nix

int main(int argc, char** argv) {
  using namespace nix;

  // The CLI has a more detailed version than the libraries; see nixVersion.
  nix_version = NIX_CLI_VERSION;
#ifndef _WIN32
  // Increase the default stack size for the evaluator and for
  // libstdc++'s std::regex.
  set_stack_size(evalStackSize);
#endif

  return handle_exceptions(argv[0], [&]() { main_wrapped(argc, argv); });
}
