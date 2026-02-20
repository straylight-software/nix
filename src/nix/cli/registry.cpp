#include "nix/fetchers/registry.h"

#include "nix/cmd/command.h"
#include "nix/expr/eval.h"
#include "nix/fetchers/fetchers.h"
#include "nix/flake/flake.h"
#include "nix/main/common-args.h"
#include "nix/main/shared.h"
#include "nix/store/store-api.h"

using namespace nix;
using namespace nix::flake;

class registry_command_t : virtual args_t {
  std::string registry_path;

  std::shared_ptr<fetchers::Registry> registry;

public:
  registry_command_t() {
    add_flag({
        .long_name = "registry",
        .description = "The registry to operate on.",
        .labels = {"registry"},
        .handler = {&registry_path},
    });
  }

  std::shared_ptr<fetchers::Registry> get_registry() {
    if (registry)
      return registry;
    if (registry_path.empty()) {
      registry = fetchers::get_user_registry(fetch_settings);
    } else {
      registry = fetchers::get_custom_registry(fetch_settings, registry_path);
    }
    return registry;
  }

  Path get_registry_path() {
    if (registry_path.empty()) {
      return fetchers::get_user_registry_path().string();
    } else {
      return registry_path;
    }
  }
};

struct cmd_registry_list_t : StoreCommand {
  std::string description() override { return "list available Nix flakes"; }

  std::string doc() override {
    return
#include "registry-list.md"
        ;
  }

  void run(nix::ref<nix::store_t> store) override {
    using namespace fetchers;

    auto registries = get_registries(fetch_settings, *store);

    for (auto& registry : registries) {
      for (auto& entry : registry->entries) {
        // FIXME: format nicely
        logger->cout("%s %s %s",
                     registry->type == Registry::flag_t     ? "flags "
                     : registry->type == Registry::User   ? "user  "
                     : registry->type == Registry::System ? "system"
                                                          : "global",
                     entry.from.toURLString(),
                     entry.to.toURLString(attrs_to_query(entry.extra_attrs)));
      }
    }
  }
};

struct cmd_registry_add_t : MixEvalArgs, command_t, registry_command_t {
  std::string from_url, to_url;

  std::string description() override { return "add/replace flake in user flake registry"; }

  std::string doc() override {
    return
#include "registry-add.md"
        ;
  }

  cmd_registry_add_t() {
    expect_arg("from-url", &from_url);
    expect_arg("to-url", &to_url);
  }

  void run() override {
    auto from_ref = parse_flake_ref(fetch_settings, from_url);
    auto to_ref = parse_flake_ref(fetch_settings, to_url);
    auto registry = get_registry();
    fetchers::Attrs extra_attrs;
    if (to_ref.subdir != "")
      extra_attrs["dir"] = to_ref.subdir;
    registry->remove(from_ref.input);
    registry->add(from_ref.input, to_ref.input, extra_attrs);
    registry->write(get_registry_path());
  }
};

struct cmd_registry_remove_t : registry_command_t, command_t {
  std::string url;

  std::string description() override { return "remove flake from user flake registry"; }

  std::string doc() override {
    return
#include "registry-remove.md"
        ;
  }

  cmd_registry_remove_t() { expect_arg("url", &url); }

  void run() override {
    auto registry = get_registry();
    registry->remove(parse_flake_ref(fetch_settings, url).input);
    registry->write(get_registry_path());
  }
};

struct cmd_registry_pin_t : registry_command_t, EvalCommand {
  std::string url;

  std::string locked;

  std::string description() override {
    return "pin a flake to its current version or to the current version of a flake URL";
  }

  std::string doc() override {
    return
#include "registry-pin.md"
        ;
  }

  cmd_registry_pin_t() {
    expect_arg("url", &url);

    expect_args({.label = "locked",
                .optional = true,
                .handler = {&locked},
                .completer = {[&](add_completions_t& completions, size_t, std::string_view prefix) {
                  complete_flake_ref(completions, getStore(), prefix);
                }}});
  }

  void run(nix::ref<nix::store_t> store) override {
    if (locked.empty())
      locked = url;
    auto registry = get_registry();
    auto ref = parse_flake_ref(fetch_settings, url);
    auto locked_ref = parse_flake_ref(fetch_settings, locked);
    auto resolved_input = locked_ref.resolve(fetch_settings, *store).input;
    auto resolved = resolved_input.get_accessor(fetch_settings, *store).second;
    if (!resolved.isLocked(fetch_settings))
      warn("flake '%s' is not locked", resolved.to_string());
    fetchers::Attrs extra_attrs;
    if (ref.subdir != "")
      extra_attrs["dir"] = ref.subdir;
    registry->remove(ref.input);
    registry->add(ref.input, resolved, extra_attrs);
    registry->write(get_registry_path());
  }
};

struct cmd_registry_resolve_t : StoreCommand {
  std::vector<std::string> urls;

  std::string description() override { return "resolve flake references using the registry"; }

  std::string doc() override {
    return
#include "registry-resolve.md"
        ;
  }

  cmd_registry_resolve_t() {
    expect_args({
        .label = "flake-refs",
        .handler = {&urls},
    });
  }

  void run(nix::ref<nix::store_t> store) override {
    for (auto& url : urls) {
      auto ref = parse_flake_ref(fetch_settings, url);
      auto resolved = ref.resolve(fetch_settings, *store);
      logger->cout("%s", resolved.to_string());
    }
  }
};

struct cmd_registry_t : NixMultiCommand {
  cmd_registry_t()
      : NixMultiCommand("registry",
                        {
                            {"list", []() { return make_ref<cmd_registry_list_t>(); }},
                            {"add", []() { return make_ref<cmd_registry_add_t>(); }},
                            {"remove", []() { return make_ref<cmd_registry_remove_t>(); }},
                            {"pin", []() { return make_ref<cmd_registry_pin_t>(); }},
                            {"resolve", []() { return make_ref<cmd_registry_resolve_t>(); }},
                        }) {}

  std::string description() override { return "manage the flake registry"; }

  std::string doc() override {
    return
#include "registry.md"
        ;
  }

  category_t category() override { return catSecondary; }
};

static auto r_cmd_registry = registerCommand<cmd_registry_t>("registry");
