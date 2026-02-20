#include "nix/cmd/command.h"
#include "nix/store/store-api.h"
#include "nix/util/archive.h"
#include "nix/util/terminal.h"

using namespace nix;

static fd_sink_t get_nar_sink() {
  auto fd = get_standard_output();
  if (is_tty(fd))
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

  void run(ref<store_t> store, const store_path_t& store_path) override {
    auto sink = get_nar_sink();
    store->nar_from_path(store_path, sink);
    sink.flush();
  }
};

static auto r_dump_path = registerCommand2<cmd_dump_path_t>({"store", "dump-path"});

struct cmd_dump_path2_t : command_t {
  Path path;

  cmd_dump_path2_t() { expect_args({.label = "path", .handler = {&path}, .completer = complete_path}); }

  std::string description() override { return "serialise a path to stdout in NAR format"; }

  std::string doc() override {
    return
#include "nar-dump-path.md"
        ;
  }

  void run() override {
    auto sink = get_nar_sink();
    dump_path(path, sink);
    sink.flush();
  }
};

struct cmd_nar_dump_path_t : cmd_dump_path2_t {
  void run() override {
    warn("'nix nar dump-path' is a deprecated alias for 'nix nar pack'");
    cmd_dump_path2_t::run();
  }
};

static auto r_cmd_nar_pack = registerCommand2<cmd_dump_path2_t>({"nar", "pack"});
static auto r_cmd_nar_dump_path = registerCommand2<cmd_nar_dump_path_t>({"nar", "dump-path"});
