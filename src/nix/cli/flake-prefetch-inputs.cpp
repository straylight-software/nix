#include <nlohmann/json.hpp>

#include "flake-command.h"
#include "nix/fetchers/fetch-to-store.h"
#include "nix/store/filetransfer.h"
#include "nix/util/exit.h"
#include "nix/util/thread-pool.h"

using namespace nix;
using namespace nix::flake;

struct cmd_flake_prefetch_inputs_t : flake_command_t {
  std::string description() override { return "fetch the inputs of a flake"; }

  std::string doc() override {
    return
#include "flake-prefetch-inputs.md"
        ;
  }

  void run(nix::ref<nix::Store> store) override {
    auto flake = lockFlake();

    thread_pool_t pool{fileTransferSettings.httpConnections};

    struct State {
      std::set<const Node*> done;
    };

    sync_t<State> state_;

    std::atomic<size_t> nrFailed{0};

    auto visit = [&](this const auto& visit, const Node& node) {
      if (!state_.lock()->done.insert(&node).second)
        return;

      if (auto lockedNode = dynamic_cast<const LockedNode*>(&node)) {
        if (lockedNode->buildTime)
          return;
        try {
          activity_t act(*logger, lvlInfo, actUnknown, fmt("fetching '%s'", lockedNode->lockedRef));
          auto accessor = lockedNode->lockedRef.input.getAccessor(fetchSettings, *store).first;
          if (!evalSettings.lazyTrees)
            fetchToStore(fetchSettings, *store, accessor, FetchMode::Copy,
                         lockedNode->lockedRef.input.getName());
        } catch (Error& e) {
          printError("%s", e.what());
          nrFailed++;
        }
      }

      for (auto& [inputName, input] : node.inputs) {
        if (auto inputNode = std::get_if<0>(&input))
          pool.enqueue(std::bind(visit, **inputNode));
      }
    };

    pool.enqueue(std::bind(visit, *flake.lockFile.root));

    pool.process();

    throw exit_t(nrFailed ? 1 : 0);
  }
};

static auto rCmdFlakePrefetchInputs =
    registerCommand2<cmd_flake_prefetch_inputs_t>({"flake", "prefetch-inputs"});
