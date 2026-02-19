#include <nlohmann/json.hpp>

#include "ls.h"
#include "nix/cmd/command.h"
#include "nix/main/shared.h"
#include "nix/store/export-import.h"
#include "nix/store/store-api.h"
#include "nix/util/archive.h"
#include "nix/util/callback.h"
#include "nix/util/fs-sink.h"

using namespace nix;

struct cmd_nario_t : NixMultiCommand {
  cmd_nario_t() : NixMultiCommand("nario", RegisterCommand::getCommandsFor({"nario"})) {}

  std::string description() override { return "operations for manipulating nario files"; }

  category_t category() override { return catUtility; }
};

static auto r_cmd_nario = registerCommand<cmd_nario_t>("nario");

struct cmd_nario_export_t : StorePathsCommand {
  unsigned int version = 0;

  cmd_nario_export_t() {
    add_flag({
        .long_name = "format",
        .description = "Version of the nario format to use. Must be `1` or `2`.",
        .labels = {"nario-format"},
        .handler = {&version},
        .required = true,
    });
  }

  std::string description() override {
    return "serialize store paths to standard output in nario format";
  }

  std::string doc() override {
    return
#include "nario-export.md"
        ;
  }

  void run(ref<Store> store, StorePaths&& store_paths) override {
    auto fd = get_standard_output();
    if (isatty(fd))
      throw UsageError("refusing to write nario to a terminal");
    fd_sink_t sink(std::move(fd));
    export_paths(*store, StorePathSet(store_paths.begin(), store_paths.end()), sink, version);
  }
};

static auto r_cmd_nario_export = registerCommand2<cmd_nario_export_t>({"nario", "export"});

static fd_source_t get_nario_source() {
  auto fd = get_standard_input();
  if (isatty(fd))
    throw UsageError("refusing to read nario from a terminal");
  return fd_source_t(std::move(fd));
}

struct cmd_nario_import_t : StoreCommand, MixNoCheckSigs {
  std::string description() override {
    return "import store paths from a nario file on standard input";
  }

  std::string doc() override {
    return
#include "nario-import.md"
        ;
  }

  void run(ref<Store> store) override {
    auto source{get_nario_source()};
    import_paths(*store, source, check_sigs);
  }
};

static auto r_cmd_nario_import = registerCommand2<cmd_nario_import_t>({"nario", "import"});

nlohmann::json list_nar(Source& source) {
  struct : file_system_object_sink_t {
    nlohmann::json root = nlohmann::json::object();

    nlohmann::json& make_object(const canon_path_t& path, std::string_view type) {
      auto* cur = &root;
      for (auto& c : path) {
        assert((*cur)["type"] == "directory");
        auto i = (*cur)["entries"].emplace(c, nlohmann::json::object()).first;
        cur = &i.value();
      }
      auto inserted = cur->emplace("type", type).second;
      assert(inserted);
      return *cur;
    }

    void create_directory(const canon_path_t& path) override {
      auto& j = make_object(path, "directory");
      j["entries"] = nlohmann::json::object();
    }

    void create_regular_file(const canon_path_t& path,
                           std::function<void(create_regular_file_sink_t&)> func) override {
      struct : create_regular_file_sink_t {
        bool executable = false;
        std::optional<uint64_t> size;

        void operator()(std::string_view data) override {}

        void preallocate_contents(uint64_t s) override { size = s; }

        void is_executable() override { executable = true; }
      } crf;

      crf.skip_contents = true;

      func(crf);

      auto& j = make_object(path, "regular");
      j.emplace("size", crf.size.value());
      if (crf.executable)
        j.emplace("executable", true);
    }

    void create_symlink(const canon_path_t& path, const std::string& target) override {
      auto& j = make_object(path, "symlink");
      j.emplace("target", target);
    }

  } parse_sink;

  parse_dump(parse_sink, source);

  return parse_sink.root;
}

void render_nar_listing(const canon_path_t& prefix, const nlohmann::json& root, bool long_listing) {
  std::function<void(const nlohmann::json& json, const canon_path_t& path)> recurse;
  recurse = [&](const nlohmann::json& json, const canon_path_t& path) {
    auto type = json["type"];

    if (long_listing) {
      auto tp = type == "regular"
                    ? (json.find("executable") != json.end() ? "-r-xr-xr-x" : "-r--r--r--")
                : type == "symlink" ? "lrwxrwxrwx"
                                    : "dr-xr-xr-x";
      auto line =
          fmt("%s %9d %s", tp, type == "regular" ? (uint64_t)json["size"] : 0, prefix / path);
      if (type == "symlink")
        line += " -> " + (std::string)json["target"];
      logger->cout(line);
    } else
      logger->cout(fmt("%s", prefix / path));

    if (type == "directory") {
      for (auto& entry : json["entries"].items()) {
        recurse(entry.value(), path / entry.key());
      }
    }
  };

  recurse(root, canon_path_t::root);
}

struct cmd_nario_list_t : command_t, MixJSON, mix_long_listing_t {
  bool list_contents = false;

  cmd_nario_list_t() {
    add_flag({
        .long_name = "recursive",
        .short_name = 'R',
        .description = "List the contents of NARs inside the nario.",
        .handler = {&list_contents, true},
    });
  }

  std::string description() override { return "list the contents of a nario file"; }

  std::string doc() override {
    return
#include "nario-list.md"
        ;
  }

  void run() override {
    struct config_t : StoreConfig {
      config_t(const Params& params) : StoreConfig(params) {}

      ref<Store> open_store() const override { abort(); }
    };

    struct listing_store_t : Store {
      std::optional<nlohmann::json> json;
      cmd_nario_list_t& cmd;

      listing_store_t(ref<const config_t> config, cmd_nario_list_t& cmd) : Store{*config}, cmd(cmd) {}

      void query_path_info_uncached(
          const StorePath& path,
          Callback<std::shared_ptr<const ValidPathInfo>> callback) noexcept override {
        callback(nullptr);
      }

      std::optional<TrustedFlag> isTrustedClient() override { return Trusted; }

      std::optional<StorePath> queryPathFromHashPart(const std::string& hash_part) override {
        return std::nullopt;
      }

      void add_to_store(const ValidPathInfo& info, Source& source, RepairFlag repair,
                      CheckSigsFlag check_sigs) override {
        std::optional<nlohmann::json> contents;
        if (cmd.list_contents)
          contents = list_nar(source);
        else
          source.skip(info.nar_size);

        if (json) {
          // FIXME: make the JSON format configurable.
          auto obj = info.to_json(this, true, PathInfoJsonFormat::V1);
          if (contents)
            obj.emplace("contents", *contents);
          json->emplace(printStorePath(info.path), std::move(obj));
        } else {
          if (contents)
            render_nar_listing(canon_path_t(printStorePath(info.path)), *contents, cmd.long_listing);
          else
            logger->cout(fmt("%s: %d bytes", printStorePath(info.path), info.nar_size));
        }
      }

      StorePath add_to_store_from_dump(Source& dump, std::string_view name,
                                   file_serialisation_method_t dump_method,
                                   ContentAddressMethod hash_method, hash_algorithm_t hash_algo,
                                   const StorePathSet& references, RepairFlag repair) override {
        unsupported("addToStoreFromDump");
      }

      void nar_from_path(const StorePath& path, Sink& sink) override { unsupported("narFromPath"); }

      void query_realisation_uncached(
          const DrvOutput&,
          Callback<std::shared_ptr<const UnkeyedRealisation>> callback) noexcept override {
        callback(nullptr);
      }

      ref<SourceAccessor> getFSAccessor(bool require_valid_path) override {
        return make_empty_source_accessor();
      }

      std::shared_ptr<SourceAccessor> getFSAccessor(const StorePath& path,
                                                    bool require_valid_path) override {
        unsupported("getFSAccessor");
      }

      void register_drv_output(const Realisation& output) override {
        unsupported("registerDrvOutput");
      }
    };

    auto source{get_nario_source()};
    auto config = make_ref<config_t>(StoreConfig::Params());
    listing_store_t lister(config, *this);
    if (json)
      lister.json = nlohmann::json::object();
    import_paths(lister, source, NoCheckSigs);
    if (json) {
      auto j = nlohmann::json::object();
      j["version"] = 1;
      j["paths"] = std::move(*lister.json);
      printJSON(j);
    }
  }
};

static auto r_cmd_nario_list = registerCommand2<cmd_nario_list_t>({"nario", "list"});
