#include "nix/store/async-path-writer.h"

#include <future>
#include <thread>

#include "nix/util/archive.h"

namespace nix {

struct async_path_writer_impl_t : AsyncPathWriter {
  ref<store_t> store;

  struct Item {
    store_path_t store_path;
    std::string contents;
    std::string name;
    Hash hash;
    store_path_set_t references;
    RepairFlag repair;
    std::promise<void> promise;
  };

  struct State {
    std::vector<Item> items;
    std::unordered_map<store_path_t, std::shared_future<void>> futures;
    bool quit = false;
  };

  sync_t<State> state_;

  std::thread worker_thread;

  std::condition_variable wakeup_cv;

  async_path_writer_impl_t(ref<store_t> store) : store(store) {
    worker_thread = std::thread([&]() {
      while (true) {
        std::vector<Item> items;

        {
          auto state(state_.lock());
          while (!state->quit && state->items.empty())
            state.wait(wakeup_cv);
          if (state->items.empty() && state->quit)
            return;
          std::swap(items, state->items);
        }

        try {
          writePaths(items);
          for (auto& item : items)
            item.promise.set_value();
        } catch (...) {
          for (auto& item : items)
            item.promise.set_exception(std::current_exception());
        }
      }
    });
  }

  virtual ~async_path_writer_impl_t() {
    state_.lock()->quit = true;
    wakeup_cv.notify_all();
    worker_thread.join();
  }

  store_path_t add_path(std::string contents, std::string name, store_path_set_t references,
                        RepairFlag repair, bool read_only) override {
    auto hash = hash_string(hash_algorithm_t::SHA256, contents);

    auto store_path = store->makeFixedOutputPathFromCA(name, TextInfo{
                                                                 .hash = hash,
                                                                 .references = references,
                                                             });

    if (!read_only) {
      auto state(state_.lock());
      std::promise<void> promise;
      state->futures.insert_or_assign(store_path, promise.get_future());
      state->items.push_back(Item{
          .store_path = store_path,
          .contents = std::move(contents),
          .name = std::move(name),
          .hash = hash,
          .references = std::move(references),
          .repair = repair,
          .promise = std::move(promise),
      });
      wakeup_cv.notify_all();
    }

    return store_path;
  }

  void waitForPath(const store_path_t& path) override {
    auto future = ({
      auto state = state_.lock();
      auto i = state->futures.find(path);
      if (i == state->futures.end())
        return;
      i->second;
    });
    future.get();
  }

  void waitForAllPaths() override {
    auto futures = ({
      auto state(state_.lock());
      std::move(state->futures);
    });
    for (auto& future : futures)
      future.second.get();
  }

  void writePaths(const std::vector<Item>& items) {
// FIXME: addMultipeToStore() shouldn't require a NAR hash.
#if 0
        store_t::PathsSource sources;
        RepairFlag repair = NoRepair;

        for (auto & item : items) {
            valid_path_info_t info{item.store_path, Hash(hash_algorithm_t::SHA256)};
            info.references = item.references;
            info.ca = content_address_t {
                .method = content_address_method_t::raw_t::Text,
                .hash = item.hash,
            };
            if (item.repair) repair = item.repair;
            auto source = sink_to_source([&](sink_t & sink)
            {
                dump_string(item.contents, sink);
            });
            sources.push_back({std::move(info), std::move(source)});
        }

        activity_t act(*logger, lvl_debug, act_unknown, fmt("adding %d paths to the store", items.size()));

        store->addMultipleToStore(std::move(sources), act, repair);
#endif

    for (auto& item : items) {
      string_source_t source(item.contents);
      auto store_path = store->add_to_store_from_dump(
          source, item.store_path.name(), file_serialisation_method_t::flat,
          content_address_method_t::raw_t::Text, hash_algorithm_t::SHA256, item.references,
          item.repair);
      assert(store_path == item.store_path);
    }
  }
};

ref<AsyncPathWriter> AsyncPathWriter::make(ref<store_t> store) {
  return make_ref<async_path_writer_impl_t>(store);
}

} // namespace nix
