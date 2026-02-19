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

class registry_command_t : virtual Args {
  std::string registry_path;

  std::shared_ptr<fetchers::Registry> registry;

public:
  registry_command_t() {
    addFlag({
        .longName = "registry",
        .description = "The registry to operate on.",
        .labels = {"registry"},
        .handler = {&registry_path},
    });
  }

  std::shared_ptr<fetchers::Registry> getRegistry() {
    if (registry)
      return registry;
    if (registry_path.empty()) {
      registry = fetchers::getUserRegistry(fetchSettings);
    } else {
      registry = fetchers::getCustomRegistry(fetchSettings, registry_path);
    }
    return registry;
  }

  Path getRegistryPath() {
    if (registry_path.empty()) {
      return fetchers::getUserRegistryPath().string();
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

  void run(nix::ref<nix::Store> store) override {
    using namespace fetchers;

    auto registries = getRegistries(fetchSettings, *store);

    for (auto& registry : registries) {
      for (auto& entry : registry->entries) {
        // FIXME: format nicely
        logger->cout("%s %s %s",
                     registry->type == Registry::flag_t     ? "flags "
                     : registry->type == Registry::User   ? "user  "
                     : registry->type == Registry::System ? "system"
                                                          : "global",
                     entry.from.toURLString(),
                     entry.to.toURLString(attrsToQuery(entry.extraAttrs)));
      }
    }
  }
};

struct cmd_registry_add_t : MixEvalArgs, command_t, registry_command_t {
  std::string fromUrl, toUrl;

  std::string description() override { return "add/replace flake in user flake registry"; }

  std::string doc() override {
    return
#include "registry-add.md"
        ;
  }

  cmd_registry_add_t() {
    expectArg("from-url", &fromUrl);
    expectArg("to-url", &toUrl);
  }

  void run() override {
    auto fromRef = parseFlakeRef(fetchSettings, fromUrl);
    auto toRef = parseFlakeRef(fetchSettings, toUrl);
    auto registry = getRegistry();
    fetchers::Attrs extraAttrs;
    if (toRef.subdir != "")
      extraAttrs["dir"] = toRef.subdir;
    registry->remove(fromRef.input);
    registry->add(fromRef.input, toRef.input, extraAttrs);
    registry->write(getRegistryPath());
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

  cmd_registry_remove_t() { expectArg("url", &url); }

  void run() override {
    auto registry = getRegistry();
    registry->remove(parseFlakeRef(fetchSettings, url).input);
    registry->write(getRegistryPath());
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
    expectArg("url", &url);

    expectArgs({.label = "locked",
                .optional = true,
                .handler = {&locked},
                .completer = {[&](add_completions_t& completions, size_t, std::string_view prefix) {
                  completeFlakeRef(completions, getStore(), prefix);
                }}});
  }

  void run(nix::ref<nix::Store> store) override {
    if (locked.empty())
      locked = url;
    auto registry = getRegistry();
    auto ref = parseFlakeRef(fetchSettings, url);
    auto lockedRef = parseFlakeRef(fetchSettings, locked);
    auto resolvedInput = lockedRef.resolve(fetchSettings, *store).input;
    auto resolved = resolvedInput.getAccessor(fetchSettings, *store).second;
    if (!resolved.isLocked(fetchSettings))
      warn("flake '%s' is not locked", resolved.to_string());
    fetchers::Attrs extraAttrs;
    if (ref.subdir != "")
      extraAttrs["dir"] = ref.subdir;
    registry->remove(ref.input);
    registry->add(ref.input, resolved, extraAttrs);
    registry->write(getRegistryPath());
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
    expectArgs({
        .label = "flake-refs",
        .handler = {&urls},
    });
  }

  void run(nix::ref<nix::Store> store) override {
    for (auto& url : urls) {
      auto ref = parseFlakeRef(fetchSettings, url);
      auto resolved = ref.resolve(fetchSettings, *store);
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

static auto rCmdRegistry = registerCommand<cmd_registry_t>("registry");
