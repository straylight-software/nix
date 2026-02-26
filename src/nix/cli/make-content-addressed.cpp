#include "nix/store/make-content-addressed.h"

#include <nlohmann/json.hpp>

#include "nix/cmd/command.h"
#include "nix/main/common-args.h"
#include "nix/store/store-open.h"

struct cmd_make_content_addressed_t : virtual nix::CopyCommand,
                                      virtual nix::StorePathsCommand,
                                      nix::MixJSON {
  cmd_make_content_addressed_t() { realiseMode = nix::Realise::Outputs; }

  std::string description() override {
    return "rewrite a path or closure to content-addressed form";
  }

  std::string doc() override {
    return
#include "make-content-addressed.md"
        ;
  }

  void run(nix::ref<nix::store_t> src_store, nix::store_paths_t&& store_paths) override {
    auto dst_store = dst_uri.empty() ? nix::open_store() : nix::open_store(dst_uri);

    auto remappings = nix::make_content_addressed(
        *src_store, *dst_store, nix::store_path_set_t(store_paths.begin(), store_paths.end()));

    if (json) {
      auto json_rewrites = nlohmann::json::object();
      for (auto& path : store_paths) {
        auto i = remappings.find(path);
        assert(i != remappings.end());
        json_rewrites[src_store->printStorePath(path)] = src_store->printStorePath(i->second);
      }
      auto json_obj = nlohmann::json::object();
      json_obj["rewrites"] = json_rewrites;
      printJSON(json_obj);
    } else {
      for (auto& path : store_paths) {
        auto i = remappings.find(path);
        assert(i != remappings.end());
        nix::notice("rewrote '%s' to '%s'", src_store->printStorePath(path),
                    src_store->printStorePath(i->second));
      }
    }
  }
};

static auto r_cmd_make_content_addressed =
    nix::registerCommand2<cmd_make_content_addressed_t>({"store", "make-content-addressed"});
