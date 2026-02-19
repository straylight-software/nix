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
    if (st.type != SourceAccessor::Type::t_regular)
      throw Error("path '%1%' is not a regular file", path.abs());
    logger->stop();

    write_full(get_standard_output(), accessor->read_file(path));
  }
};

struct cmd_cat_store_t : StoreCommand, mix_cat_t {
  std::string path;

  cmd_cat_store_t() { expect_args({.label = "path", .handler = {&path}, .completer = complete_path}); }

  std::string description() override {
    return "print the contents of a file in the Nix store on stdout";
  }

  std::string doc() override {
    return
#include "store-cat.md"
        ;
  }

  void run(ref<Store> store) override {
    auto [store_path, rest] = store->toStorePath(path);
    cat(store->requireStoreObjectAccessor(store_path), canon_path_t{rest});
  }
};

struct cmd_cat_nar_t : StoreCommand, mix_cat_t {
  Path nar_path;

  std::string path;

  cmd_cat_nar_t() {
    expect_args({.label = "nar", .handler = {&nar_path}, .completer = complete_path});
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

  void run(ref<Store> store) override {
    auto_close_fd_t fd = to_descriptor(open(nar_path.c_str(), O_RDONLY));
    if (!fd)
      throw sys_error_t("opening NAR file '%s'", nar_path);
    auto source = fd_source_t{fd.get()};

    struct cat_regular_file_sink_t : null_file_system_object_sink_t {
      canon_path_t needed_path = canon_path_t::root;
      bool found = false;

      void create_regular_file(const canon_path_t& path,
                             std::function<void(create_regular_file_sink_t&)> crf) override {
        struct : create_regular_file_sink_t, fd_sink_t {
          void is_executable() override {}
        } crf_sink;

        crf_sink.fd = INVALID_DESCRIPTOR;

        if (path == needed_path) {
          logger->stop();
          crf_sink.skip_contents = false;
          crf_sink.fd = get_standard_output();
          found = true;
        } else {
          crf_sink.skip_contents = true;
        }

        crf(crf_sink);
      }
    } sink;

    sink.needed_path = canon_path_t(path);
    /* NOTE: We still parse the whole file to validate that it's a correct NAR. */
    parse_dump(sink, source);

    if (!sink.found)
      throw Error("NAR does not contain regular file '%1%'", path);
  }
};

static auto r_cmd_cat_store = registerCommand2<cmd_cat_store_t>({"store", "cat"});
static auto r_cmd_cat_nar = registerCommand2<cmd_cat_nar_t>({"nar", "cat"});
