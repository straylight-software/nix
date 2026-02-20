#include "nix/store/make-content-addressed.h"

#include <nlohmann/json.hpp>

#include "nix/cmd/command.h"
#include "nix/main/common-args.h"
#include "nix/store/store-open.h"

using namespace nix;

using nlohmann::json;

struct cmd_make_content_addressed_t : virtual CopyCommand, virtual StorePathsCommand, MixJSON {
  cmd_make_content_addressed_t() { realiseMode = Realise::Outputs; }

  std::string description() override {
    return "rewrite a path or closure to content-addressed form";
  }

  std::string doc() override {
    return
#include "make-content-addressed.md"
        ;
  }

  void run(ref<store_t> src_store, store_paths_t&& store_paths) override {
    auto dst_store = dst_uri.empty() ? open_store() : open_store(dst_uri);

    auto remappings = make_content_addressed(*src_store, *dst_store,
                                           store_path_set_t(store_paths.begin(), store_paths.end()));

    if (json) {
      auto json_rewrites = json::object();
      for (auto& path : store_paths) {
        auto i = remappings.find(path);
        assert(i != remappings.end());
        json_rewrites[src_store->printStorePath(path)] = src_store->printStorePath(i->second);
      }
      auto json = json::object();
      json["rewrites"] = json_rewrites;
      printJSON(json);
    } else {
      for (auto& path : store_paths) {
        auto i = remappings.find(path);
        assert(i != remappings.end());
        notice("rewrote '%s' to '%s'", src_store->printStorePath(path),
               src_store->printStorePath(i->second));
      }
    }
  }
};

static auto r_cmd_make_content_addressed =
    registerCommand2<cmd_make_content_addressed_t>({"store", "make-content-addressed"});
