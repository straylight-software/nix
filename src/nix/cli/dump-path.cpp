#include "nix/cmd/command.h"
#include "nix/store/store-api.h"
#include "nix/util/archive.h"
#include "nix/util/terminal.h"

using namespace nix;

static fd_sink_t getNarSink() {
  auto fd = getStandardOutput();
  if (isTTY(fd))
    throw UsageError("refusing to write NAR to a terminal");
  return fd_sink_t(std::move(fd));
}

struct cmd_dump_path_t : StorePathCommand {
  std::string description() override { return "serialise a store path to stdout in NAR format"; }

  std::string doc() override {
    return
#include "store-dump-path.md"
        ;
  }

  void run(ref<Store> store, const StorePath& storePath) override {
    auto sink = getNarSink();
    store->narFromPath(storePath, sink);
    sink.flush();
  }
};

static auto rDumpPath = registerCommand2<cmd_dump_path_t>({"store", "dump-path"});

struct cmd_dump_path2_t : command_t {
  Path path;

  cmd_dump_path2_t() { expectArgs({.label = "path", .handler = {&path}, .completer = completePath}); }

  std::string description() override { return "serialise a path to stdout in NAR format"; }

  std::string doc() override {
    return
#include "nar-dump-path.md"
        ;
  }

  void run() override {
    auto sink = getNarSink();
    dumpPath(path, sink);
    sink.flush();
  }
};

struct cmd_nar_dump_path_t : cmd_dump_path2_t {
  void run() override {
    warn("'nix nar dump-path' is a deprecated alias for 'nix nar pack'");
    cmd_dump_path2_t::run();
  }
};

static auto rCmdNarPack = registerCommand2<cmd_dump_path2_t>({"nar", "pack"});
static auto rCmdNarDumpPath = registerCommand2<cmd_nar_dump_path_t>({"nar", "dump-path"});
