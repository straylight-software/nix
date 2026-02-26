#include <fcntl.h>

#include <nlohmann/json.hpp>

#include "nix/cmd/command.h"
#include "nix/store/store-api.h"
#include "nix/util/archive.h"
#include "nix/util/nar-accessor.h"
#include "nix/util/serialise.h"
#include "nix/util/source-accessor.h"

struct mix_cat_t : virtual nix::args_t {
  void cat(nix::ref<nix::source_accessor_t> accessor, nix::canon_path_t path) {
    auto st = accessor->lstat(path);
    if (st.type != nix::source_accessor_t::Type::t_regular)
      throw nix::Error("path '%1%' is not a regular file", path.abs());
    nix::logger->stop();

    nix::write_full(nix::get_standard_output(), accessor->read_file(path));
  }
};

struct cmd_cat_store_t : nix::StoreCommand, mix_cat_t {
  std::string path;

  cmd_cat_store_t() {
    expect_args({.label = "path", .handler = {&path}, .completer = nix::complete_path});
  }

  std::string description() override {
    return "print the contents of a file in the Nix store on stdout";
  }

  std::string doc() override {
    return
#include "store-cat.md"
        ;
  }

  void run(nix::ref<nix::store_t> store) override {
    auto [store_path, rest] = store->toStorePath(path);
    cat(store->requireStoreObjectAccessor(store_path), nix::canon_path_t{rest});
  }
};

struct cmd_cat_nar_t : nix::StoreCommand, mix_cat_t {
  nix::Path nar_path;

  std::string path;

  cmd_cat_nar_t() {
    expect_args({.label = "nar", .handler = {&nar_path}, .completer = nix::complete_path});
    expect_arg("path", &path);
  }

  std::string description() override {
    return "print the contents of a file inside a NAR file on stdout";
  }

  std::string doc() override {
    return
#include "nar-cat.md"
        ;
  }

  void run(nix::ref<nix::store_t> store) override {
    nix::auto_close_fd_t fd = nix::to_descriptor(open(nar_path.c_str(), O_RDONLY));
    if (!fd)
      throw nix::sys_error_t("opening NAR file '%s'", nar_path);
    auto source = nix::fd_source_t{fd.get()};

    struct cat_regular_file_sink_t : nix::null_file_system_object_sink_t {
      nix::canon_path_t needed_path = nix::canon_path_t::root;
      bool found = false;

      void create_regular_file(const nix::canon_path_t& path,
                               std::function<void(nix::create_regular_file_sink_t&)> crf) override {
        struct : nix::create_regular_file_sink_t, nix::fd_sink_t {
          void is_executable() override {}
        } crf_sink;

        crf_sink.set_fd(nix::INVALID_DESCRIPTOR);

        if (path == needed_path) {
          nix::logger->stop();
          crf_sink.skip_contents = false;
          crf_sink.set_fd(nix::get_standard_output());
          found = true;
        } else {
          crf_sink.skip_contents = true;
        }

        crf(crf_sink);
      }
    } sink;

    sink.needed_path = nix::canon_path_t(path);
    /* NOTE: We still parse the whole file to validate that it's a correct NAR. */
    nix::parse_dump(sink, source);

    if (!sink.found)
      throw nix::Error("NAR does not contain regular file '%1%'", path);
  }
};

static auto r_cmd_cat_store = nix::registerCommand2<cmd_cat_store_t>({"store", "cat"});
static auto r_cmd_cat_nar = nix::registerCommand2<cmd_cat_nar_t>({"nar", "cat"});
