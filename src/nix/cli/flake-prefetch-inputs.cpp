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

  void run(nix::ref<nix::store_t> store) override {
    auto flake = lock_flake();

    thread_pool_t pool{file_transfer_settings.httpConnections};

    struct State {
      std::set<const Node*> done;
    };

    sync_t<State> state_;

    std::atomic<size_t> nrFailed{0};

    auto visit = [&](this const auto& visit, const Node& node) {
      if (!state_.lock()->done.insert(&node).second)
        return;

      if (auto locked_node = dynamic_cast<const LockedNode*>(&node)) {
        if (locked_node->buildTime)
          return;
        try {
          activity_t act(*logger, lvl_info, act_unknown,
                         fmt("fetching '%s'", locked_node->locked_ref));
          auto accessor = locked_node->locked_ref.input.get_accessor(fetch_settings, *store).first;
          if (!eval_settings.lazyTrees)
            fetch_to_store(fetch_settings, *store, accessor, FetchMode::Copy,
                           locked_node->locked_ref.input.get_name());
        } catch (Error& e) {
          printError("%s", e.what());
          nrFailed++;
        }
      }

      for (auto& [inputName, input] : node.inputs) {
        if (auto input_node = std::get_if<0>(&input))
          pool.enqueue(std::bind(visit, **input_node));
      }
    };

    pool.enqueue(std::bind(visit, *flake.lock_file.root));

    pool.process();

    throw exit_t(nrFailed ? 1 : 0);
  }
};

static auto r_cmd_flake_prefetch_inputs =
    registerCommand2<cmd_flake_prefetch_inputs_t>({"flake", "prefetch-inputs"});
