#include "nix/fetchers/registry.h"

#include "nix/cmd/command.h"
#include "nix/expr/eval.h"
#include "nix/fetchers/fetchers.h"
#include "nix/flake/flake.h"
#include "nix/main/common-args.h"
#include "nix/main/shared.h"
#include "nix/store/store-api.h"

struct registry_command_t : virtual nix::args_t {
  std::string registry_path;

  std::shared_ptr<nix::fetchers::Registry> registry;

  registry_command_t() {
    add_flag({
        .long_name = "registry",
        .description = "The registry to operate on.",
        .labels = {"registry"},
        .handler = {&registry_path},
    });
  }

  std::shared_ptr<nix::fetchers::Registry> get_registry() {
    if (registry) {
      return registry;
    }
    if (registry_path.empty()) {
      registry = nix::fetchers::get_user_registry(nix::fetch_settings);
    } else {
      registry = nix::fetchers::get_custom_registry(nix::fetch_settings, registry_path);
    }
    return registry;
  }

  nix::Path get_registry_path() {
    if (registry_path.empty()) {
      return nix::fetchers::get_user_registry_path().string();
    } else {
      return registry_path;
    }
  }
};

struct cmd_registry_list_t : nix::StoreCommand {
  std::string description() override { return "list available Nix flakes"; }

  std::string doc() override {
    return
#include "registry-list.md"
        ;
  }

  void run(nix::ref<nix::store_t> store) override {
    auto registries = nix::fetchers::get_registries(nix::fetch_settings, *store);

    for (auto& registry : registries) {
      for (auto& entry : registry->entries) {
        // FIXME: format nicely
        nix::logger->cout("%s %s %s",
                          registry->type == nix::fetchers::Registry::flag_t   ? "flags "
                          : registry->type == nix::fetchers::Registry::User   ? "user  "
                          : registry->type == nix::fetchers::Registry::System ? "system"
                                                                              : "global",
                          entry.from.toURLString(),
                          entry.to.toURLString(nix::fetchers::attrs_to_query(entry.extra_attrs)));
      }
    }
  }
};

struct cmd_registry_add_t : nix::MixEvalArgs, nix::command_t, registry_command_t {
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
    auto from_ref = nix::parse_flake_ref(nix::fetch_settings, from_url);
    auto to_ref = nix::parse_flake_ref(nix::fetch_settings, to_url);
    auto registry = get_registry();
    nix::fetchers::Attrs extra_attrs;
    if (to_ref.subdir != "") {
      extra_attrs["dir"] = to_ref.subdir;
    }
    registry->remove(from_ref.input);
    registry->add(from_ref.input, to_ref.input, extra_attrs);
    registry->write(get_registry_path());
  }
};

struct cmd_registry_remove_t : registry_command_t, nix::command_t {
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
    registry->remove(nix::parse_flake_ref(nix::fetch_settings, url).input);
    registry->write(get_registry_path());
  }
};

struct cmd_registry_pin_t : registry_command_t, nix::EvalCommand {
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

    expect_args(
        {.label = "locked",
         .optional = true,
         .handler = {&locked},
         .completer = {[&](nix::add_completions_t& completions, size_t, std::string_view prefix) {
           nix::complete_flake_ref(completions, getStore(), prefix);
         }}});
  }

  void run(nix::ref<nix::store_t> store) override {
    if (locked.empty()) {
      locked = url;
    }
    auto registry = get_registry();
    auto ref = nix::parse_flake_ref(nix::fetch_settings, url);
    auto locked_ref = nix::parse_flake_ref(nix::fetch_settings, locked);
    auto resolved_input = locked_ref.resolve(nix::fetch_settings, *store).input;
    auto resolved = resolved_input.get_accessor(nix::fetch_settings, *store).second;
    if (!resolved.isLocked(nix::fetch_settings)) {
      nix::warn("flake '%s' is not locked", resolved.to_string());
    }
    nix::fetchers::Attrs extra_attrs;
    if (ref.subdir != "") {
      extra_attrs["dir"] = ref.subdir;
    }
    registry->remove(ref.input);
    registry->add(ref.input, resolved, extra_attrs);
    registry->write(get_registry_path());
  }
};

struct cmd_registry_resolve_t : nix::StoreCommand {
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
      auto ref = nix::parse_flake_ref(nix::fetch_settings, url);
      auto resolved = ref.resolve(nix::fetch_settings, *store);
      nix::logger->cout("%s", resolved.to_string());
    }
  }
};

struct cmd_registry_t : nix::NixMultiCommand {
  cmd_registry_t()
      : NixMultiCommand("registry",
                        {
                            {"list", []() { return nix::make_ref<cmd_registry_list_t>(); }},
                            {"add", []() { return nix::make_ref<cmd_registry_add_t>(); }},
                            {"remove", []() { return nix::make_ref<cmd_registry_remove_t>(); }},
                            {"pin", []() { return nix::make_ref<cmd_registry_pin_t>(); }},
                            {"resolve", []() { return nix::make_ref<cmd_registry_resolve_t>(); }},
                        }) {}

  std::string description() override { return "manage the flake registry"; }

  std::string doc() override {
    return
#include "registry.md"
        ;
  }

  category_t category() override { return nix::catSecondary; }
};

static auto r_cmd_registry = nix::registerCommand<cmd_registry_t>("registry");
