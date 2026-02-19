#include <nlohmann/json.hpp>

#include "nix/cmd/command.h"
#include "nix/store/store-api.h"
#include "nix/util/archive.h"
#include "nix/util/nar-accessor.h"
#include "nix/util/serialise.h"
#include "nix/util/source-accessor.h"

using namespace nix;

struct mix_cat_t : virtual Args {
  void cat(ref<SourceAccessor> accessor, canon_path_t path) {
    auto st = accessor->lstat(path);
    if (st.type != SourceAccessor::Type::tRegular)
      throw Error("path '%1%' is not a regular file", path.abs());
    logger->stop();

    writeFull(getStandardOutput(), accessor->readFile(path));
  }
};

struct cmd_cat_store_t : StoreCommand, mix_cat_t {
  std::string path;

  cmd_cat_store_t() { expectArgs({.label = "path", .handler = {&path}, .completer = completePath}); }

  std::string description() override {
    return "print the contents of a file in the Nix store on stdout";
  }

  std::string doc() override {
    return
#include "store-cat.md"
        ;
  }

  void run(ref<Store> store) override {
    auto [storePath, rest] = store->toStorePath(path);
    cat(store->requireStoreObjectAccessor(storePath), canon_path_t{rest});
  }
};

struct cmd_cat_nar_t : StoreCommand, mix_cat_t {
  Path narPath;

  std::string path;

  cmd_cat_nar_t() {
    expectArgs({.label = "nar", .handler = {&narPath}, .completer = completePath});
    expectArg("path", &path);
  }

  std::string description() override {
    return "print the contents of a file inside a NAR file on stdout";
  }

  std::string doc() override {
    return
#include "nar-cat.md"
        ;
  }

  void run(ref<Store> store) override {
    auto_close_fd_t fd = toDescriptor(open(narPath.c_str(), O_RDONLY));
    if (!fd)
      throw sys_error_t("opening NAR file '%s'", narPath);
    auto source = fd_source_t{fd.get()};

    struct cat_regular_file_sink_t : null_file_system_object_sink_t {
      canon_path_t neededPath = canon_path_t::root;
      bool found = false;

      void createRegularFile(const canon_path_t& path,
                             std::function<void(create_regular_file_sink_t&)> crf) override {
        struct : create_regular_file_sink_t, fd_sink_t {
          void isExecutable() override {}
        } crfSink;

        crfSink.fd = INVALID_DESCRIPTOR;

        if (path == neededPath) {
          logger->stop();
          crfSink.skipContents = false;
          crfSink.fd = getStandardOutput();
          found = true;
        } else {
          crfSink.skipContents = true;
        }

        crf(crfSink);
      }
    } sink;

    sink.neededPath = canon_path_t(path);
    /* NOTE: We still parse the whole file to validate that it's a correct NAR. */
    parseDump(sink, source);

    if (!sink.found)
      throw Error("NAR does not contain regular file '%1%'", path);
  }
};

static auto rCmdCatStore = registerCommand2<cmd_cat_store_t>({"store", "cat"});
static auto rCmdCatNar = registerCommand2<cmd_cat_nar_t>({"nar", "cat"});
