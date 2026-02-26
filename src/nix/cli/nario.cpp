#include <nlohmann/json.hpp>

#include "ls.h"
#include "nix/cmd/command.h"
#include "nix/main/shared.h"
#include "nix/store/export-import.h"
#include "nix/store/store-api.h"
#include "nix/util/archive.h"
#include "nix/util/callback.h"
#include "nix/util/fs-sink.h"

struct cmd_nario_t : nix::NixMultiCommand {
  cmd_nario_t() : NixMultiCommand("nario", nix::RegisterCommand::getCommandsFor({"nario"})) {}

  std::string description() override { return "operations for manipulating nario files"; }

  nix::category_t category() override { return nix::catUtility; }
};

static auto r_cmd_nario = nix::registerCommand<cmd_nario_t>("nario");

struct cmd_nario_export_t : nix::StorePathsCommand {
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

  void run(nix::ref<nix::store_t> store, nix::store_paths_t&& store_paths) override {
    auto fd = nix::get_standard_output();
    if (isatty(fd))
      throw nix::UsageError("refusing to write nario to a terminal");
    nix::fd_sink_t sink(std::move(fd));
    nix::export_paths(*store, nix::store_path_set_t(store_paths.begin(), store_paths.end()), sink,
                      version);
  }
};

static auto r_cmd_nario_export = nix::registerCommand2<cmd_nario_export_t>({"nario", "export"});

static nix::fd_source_t get_nario_source() {
  auto fd = nix::get_standard_input();
  if (isatty(fd))
    throw nix::UsageError("refusing to read nario from a terminal");
  return nix::fd_source_t(std::move(fd));
}

struct cmd_nario_import_t : nix::StoreCommand, nix::MixNoCheckSigs {
  std::string description() override {
    return "import store paths from a nario file on standard input";
  }

  std::string doc() override {
    return
#include "nario-import.md"
        ;
  }

  void run(nix::ref<nix::store_t> store) override {
    auto source{get_nario_source()};
    nix::import_paths(*store, source, check_sigs);
  }
};

static auto r_cmd_nario_import = nix::registerCommand2<cmd_nario_import_t>({"nario", "import"});

nlohmann::json list_nar(nix::source_t& source) {
  struct : nix::file_system_object_sink_t {
    nlohmann::json root = nlohmann::json::object();

    nlohmann::json& make_object(const nix::canon_path_t& path, std::string_view type) {
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

    void create_directory(const nix::canon_path_t& path) override {
      auto& j = make_object(path, "directory");
      j["entries"] = nlohmann::json::object();
    }

    void create_regular_file(const nix::canon_path_t& path,
                             std::function<void(nix::create_regular_file_sink_t&)> func) override {
      struct : nix::create_regular_file_sink_t {
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

    void create_symlink(const nix::canon_path_t& path, const std::string& target) override {
      auto& j = make_object(path, "symlink");
      j.emplace("target", target);
    }

  } parse_sink;

  nix::parse_dump(parse_sink, source);

  return parse_sink.root;
}

void render_nar_listing(const nix::canon_path_t& prefix, const nlohmann::json& root,
                        bool long_listing) {
  std::function<void(const nlohmann::json& json, const nix::canon_path_t& path)> recurse;
  recurse = [&](const nlohmann::json& json, const nix::canon_path_t& path) {
    auto type = json["type"];

    if (long_listing) {
      auto tp = type == "regular"
                    ? (json.find("executable") != json.end() ? "-r-xr-xr-x" : "-r--r--r--")
                : type == "symlink" ? "lrwxrwxrwx"
                                    : "dr-xr-xr-x";
      auto line =
          nix::fmt("%s %9d %s", tp, type == "regular" ? (uint64_t)json["size"] : 0, prefix / path);
      if (type == "symlink")
        line += " -> " + (std::string)json["target"];
      nix::logger->cout(line);
    } else
      nix::logger->cout(nix::fmt("%s", prefix / path));

    if (type == "directory") {
      for (auto& entry : json["entries"].items()) {
        recurse(entry.value(), path / entry.key());
      }
    }
  };

  recurse(root, nix::canon_path_t::root);
}

struct cmd_nario_list_t : nix::command_t, nix::MixJSON, nix::mix_long_listing_t {
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
    struct config_t : nix::store_config_t {
      config_t(const Params& params) : store_config_t(params) {}

      nix::ref<nix::store_t> open_store() const override { abort(); }
    };

    struct listing_store_t : nix::store_t {
      std::optional<nlohmann::json> json;
      cmd_nario_list_t& cmd;

      listing_store_t(nix::ref<const config_t> config, cmd_nario_list_t& cmd)
          : store_t{*config}, cmd(cmd) {}

      void query_path_info_uncached(
          const nix::store_path_t& path,
          nix::Callback<std::shared_ptr<const nix::valid_path_info_t>> callback) noexcept override {
        callback(nullptr);
      }

      std::optional<nix::TrustedFlag> isTrustedClient() override { return nix::Trusted; }

      std::optional<nix::store_path_t>
      queryPathFromHashPart(const std::string& hash_part) override {
        return std::nullopt;
      }

      void add_to_store(const nix::valid_path_info_t& info, nix::source_t& source,
                        nix::RepairFlag repair, nix::CheckSigsFlag check_sigs) override {
        std::optional<nlohmann::json> contents;
        if (cmd.list_contents)
          contents = list_nar(source);
        else
          source.skip(info.nar_size);

        if (json) {
          // FIXME: make the JSON format configurable.
          auto obj = info.to_json(this, true, nix::PathInfoJsonFormat::V1);
          if (contents)
            obj.emplace("contents", *contents);
          json->emplace(printStorePath(info.path), std::move(obj));
        } else {
          if (contents)
            render_nar_listing(nix::canon_path_t(printStorePath(info.path)), *contents,
                               cmd.long_listing);
          else
            nix::logger->cout(nix::fmt("%s: %d bytes", printStorePath(info.path), info.nar_size));
        }
      }

      nix::store_path_t add_to_store_from_dump(nix::source_t& dump, std::string_view name,
                                               nix::file_serialisation_method_t dump_method,
                                               nix::content_address_method_t hash_method,
                                               nix::hash_algorithm_t hash_algo,
                                               const nix::store_path_set_t& references,
                                               nix::RepairFlag repair) override {
        unsupported("addToStoreFromDump");
      }

      void nar_from_path(const nix::store_path_t& path, nix::sink_t& sink) override {
        unsupported("narFromPath");
      }

      void query_realisation_uncached(const nix::DrvOutput&,
                                      nix::Callback<std::shared_ptr<const nix::UnkeyedRealisation>>
                                          callback) noexcept override {
        callback(nullptr);
      }

      nix::ref<nix::source_accessor_t> getFSAccessor(bool require_valid_path) override {
        return nix::make_empty_source_accessor();
      }

      std::shared_ptr<nix::source_accessor_t> getFSAccessor(const nix::store_path_t& path,
                                                            bool require_valid_path) override {
        unsupported("getFSAccessor");
      }

      void register_drv_output(const nix::realisation_t& output) override {
        unsupported("registerDrvOutput");
      }
    };

    auto source{get_nario_source()};
    auto config = nix::make_ref<config_t>(nix::store_config_t::Params());
    listing_store_t lister(config, *this);
    if (json)
      lister.json = nlohmann::json::object();
    nix::import_paths(lister, source, nix::NoCheckSigs);
    if (json) {
      auto j = nlohmann::json::object();
      j["version"] = 1;
      j["paths"] = std::move(*lister.json);
      printJSON(j);
    }
  }
};

static auto r_cmd_nario_list = nix::registerCommand2<cmd_nario_list_t>({"nario", "list"});
