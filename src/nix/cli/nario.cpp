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

static auto rCmdNario = registerCommand<cmd_nario_t>("nario");

struct cmd_nario_export_t : StorePathsCommand {
  unsigned int version = 0;

  cmd_nario_export_t() {
    addFlag({
        .longName = "format",
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

  void run(ref<Store> store, StorePaths&& storePaths) override {
    auto fd = getStandardOutput();
    if (isatty(fd))
      throw UsageError("refusing to write nario to a terminal");
    fd_sink_t sink(std::move(fd));
    exportPaths(*store, StorePathSet(storePaths.begin(), storePaths.end()), sink, version);
  }
};

static auto rCmdNarioExport = registerCommand2<cmd_nario_export_t>({"nario", "export"});

static fd_source_t getNarioSource() {
  auto fd = getStandardInput();
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
    auto source{getNarioSource()};
    importPaths(*store, source, checkSigs);
  }
};

static auto rCmdNarioImport = registerCommand2<cmd_nario_import_t>({"nario", "import"});

nlohmann::json listNar(Source& source) {
  struct : file_system_object_sink_t {
    nlohmann::json root = nlohmann::json::object();

    nlohmann::json& makeObject(const canon_path_t& path, std::string_view type) {
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

    void createDirectory(const canon_path_t& path) override {
      auto& j = makeObject(path, "directory");
      j["entries"] = nlohmann::json::object();
    }

    void createRegularFile(const canon_path_t& path,
                           std::function<void(create_regular_file_sink_t&)> func) override {
      struct : create_regular_file_sink_t {
        bool executable = false;
        std::optional<uint64_t> size;

        void operator()(std::string_view data) override {}

        void preallocateContents(uint64_t s) override { size = s; }

        void isExecutable() override { executable = true; }
      } crf;

      crf.skipContents = true;

      func(crf);

      auto& j = makeObject(path, "regular");
      j.emplace("size", crf.size.value());
      if (crf.executable)
        j.emplace("executable", true);
    }

    void createSymlink(const canon_path_t& path, const std::string& target) override {
      auto& j = makeObject(path, "symlink");
      j.emplace("target", target);
    }

  } parseSink;

  parseDump(parseSink, source);

  return parseSink.root;
}

void renderNarListing(const canon_path_t& prefix, const nlohmann::json& root, bool longListing) {
  std::function<void(const nlohmann::json& json, const canon_path_t& path)> recurse;
  recurse = [&](const nlohmann::json& json, const canon_path_t& path) {
    auto type = json["type"];

    if (longListing) {
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
  bool listContents = false;

  cmd_nario_list_t() {
    addFlag({
        .longName = "recursive",
        .shortName = 'R',
        .description = "List the contents of NARs inside the nario.",
        .handler = {&listContents, true},
    });
  }

  std::string description() override { return "list the contents of a nario file"; }

  std::string doc() override {
    return
#include "nario-list.md"
        ;
  }

  void run() override {
    struct Config : StoreConfig {
      Config(const Params& params) : StoreConfig(params) {}

      ref<Store> openStore() const override { abort(); }
    };

    struct listing_store_t : Store {
      std::optional<nlohmann::json> json;
      cmd_nario_list_t& cmd;

      listing_store_t(ref<const Config> config, cmd_nario_list_t& cmd) : Store{*config}, cmd(cmd) {}

      void queryPathInfoUncached(
          const StorePath& path,
          Callback<std::shared_ptr<const ValidPathInfo>> callback) noexcept override {
        callback(nullptr);
      }

      std::optional<TrustedFlag> isTrustedClient() override { return Trusted; }

      std::optional<StorePath> queryPathFromHashPart(const std::string& hashPart) override {
        return std::nullopt;
      }

      void addToStore(const ValidPathInfo& info, Source& source, RepairFlag repair,
                      CheckSigsFlag checkSigs) override {
        std::optional<nlohmann::json> contents;
        if (cmd.listContents)
          contents = listNar(source);
        else
          source.skip(info.narSize);

        if (json) {
          // FIXME: make the JSON format configurable.
          auto obj = info.toJSON(this, true, PathInfoJsonFormat::V1);
          if (contents)
            obj.emplace("contents", *contents);
          json->emplace(printStorePath(info.path), std::move(obj));
        } else {
          if (contents)
            renderNarListing(canon_path_t(printStorePath(info.path)), *contents, cmd.longListing);
          else
            logger->cout(fmt("%s: %d bytes", printStorePath(info.path), info.narSize));
        }
      }

      StorePath addToStoreFromDump(Source& dump, std::string_view name,
                                   file_serialisation_method_t dumpMethod,
                                   ContentAddressMethod hashMethod, hash_algorithm_t hashAlgo,
                                   const StorePathSet& references, RepairFlag repair) override {
        unsupported("addToStoreFromDump");
      }

      void narFromPath(const StorePath& path, Sink& sink) override { unsupported("narFromPath"); }

      void queryRealisationUncached(
          const DrvOutput&,
          Callback<std::shared_ptr<const UnkeyedRealisation>> callback) noexcept override {
        callback(nullptr);
      }

      ref<SourceAccessor> getFSAccessor(bool requireValidPath) override {
        return makeEmptySourceAccessor();
      }

      std::shared_ptr<SourceAccessor> getFSAccessor(const StorePath& path,
                                                    bool requireValidPath) override {
        unsupported("getFSAccessor");
      }

      void registerDrvOutput(const Realisation& output) override {
        unsupported("registerDrvOutput");
      }
    };

    auto source{getNarioSource()};
    auto config = make_ref<Config>(StoreConfig::Params());
    listing_store_t lister(config, *this);
    if (json)
      lister.json = nlohmann::json::object();
    importPaths(lister, source, NoCheckSigs);
    if (json) {
      auto j = nlohmann::json::object();
      j["version"] = 1;
      j["paths"] = std::move(*lister.json);
      printJSON(j);
    }
  }
};

static auto rCmdNarioList = registerCommand2<cmd_nario_list_t>({"nario", "list"});
