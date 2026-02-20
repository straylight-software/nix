#include "ls.h"

#include <fcntl.h>

#include <nlohmann/json.hpp>

#include "nix/cmd/command.h"
#include "nix/main/common-args.h"
#include "nix/store/store-api.h"
#include "nix/util/nar-accessor.h"

using namespace nix;

struct mix_ls_t : virtual args_t, MixJSON, mix_long_listing_t {
  bool recursive = false;
  bool show_directory = false;

  mix_ls_t() {
    add_flag({
        .long_name = "recursive",
        .short_name = 'R',
        .description = "List subdirectories recursively.",
        .handler = {&recursive, true},
    });

    add_flag({
        .long_name = "directory",
        .short_name = 'd',
        .description = "Show directories rather than their contents.",
        .handler = {&show_directory, true},
    });
  }

  void list_text(ref<source_accessor_t> accessor, canon_path_t path) {
    std::function<void(const source_accessor_t::stat_t&, const canon_path_t&, std::string_view,
                       bool)>
        do_path;

    auto show_file = [&](const canon_path_t& cur_path, std::string_view rel_path) {
      if (long_listing) {
        auto st = accessor->lstat(cur_path);
        std::string tp = st.type == source_accessor_t::Type::t_regular
                             ? (st.is_executable ? "-r-xr-xr-x" : "-r--r--r--")
                         : st.type == source_accessor_t::Type::t_symlink ? "lrwxrwxrwx"
                                                                         : "dr-xr-xr-x";
        auto line = fmt("%s %9d %s", tp, st.file_size.value_or(0), rel_path);
        if (st.type == source_accessor_t::Type::t_symlink)
          line += " -> " + accessor->read_link(cur_path);
        logger->cout(line);
        if (recursive && st.type == source_accessor_t::Type::t_directory)
          do_path(st, cur_path, rel_path, false);
      } else {
        logger->cout(rel_path);
        if (recursive) {
          auto st = accessor->lstat(cur_path);
          if (st.type == source_accessor_t::Type::t_directory)
            do_path(st, cur_path, rel_path, false);
        }
      }
    };

    do_path = [&](const source_accessor_t::stat_t& st, const canon_path_t& cur_path,
                  std::string_view rel_path, bool show_directory) {
      if (st.type == source_accessor_t::Type::t_directory && !show_directory) {
        auto names = accessor->read_directory(cur_path);
        for (auto& [name, type] : names)
          show_file(cur_path / name, rel_path + "/" + name);
      } else
        show_file(cur_path, rel_path);
    };

    auto st = accessor->lstat(path);
    do_path(st, path,
            st.type == source_accessor_t::Type::t_directory ? "." : path.base_name().value_or(""),
            show_directory);
  }

  void list(ref<source_accessor_t> accessor, canon_path_t path) {
    if (json) {
      if (show_directory)
        throw UsageError("'--directory' is useless with '--json'");
      nlohmann::json j;
      if (recursive)
        j = list_nar_deep(*accessor, path);
      else
        j = list_nar_shallow(*accessor, path);
      logger->cout("%s", j.dump());
    } else
      list_text(accessor, std::move(path));
  }
};

struct cmd_ls_store_t : StoreCommand, mix_ls_t {
  std::string path;

  cmd_ls_store_t() {
    expect_args({.label = "path", .handler = {&path}, .completer = complete_path});
  }

  std::string description() override { return "show information about a path in the Nix store"; }

  std::string doc() override {
    return
#include "store-ls.md"
        ;
  }

  void run(ref<store_t> store) override {
    auto [store_path, rest] = store->toStorePath(path);
    list(store->requireStoreObjectAccessor(store_path), canon_path_t{rest});
  }
};

struct cmd_ls_nar_t : command_t, mix_ls_t {
  Path nar_path;

  std::string path;

  cmd_ls_nar_t() {
    expect_args({.label = "nar", .handler = {&nar_path}, .completer = complete_path});
    expect_arg("path", &path);
  }

  std::string doc() override {
    return
#include "nar-ls.md"
        ;
  }

  std::string description() override { return "show information about a path inside a NAR file"; }

  void run() override {
    auto_close_fd_t fd = to_descriptor(open(nar_path.c_str(), O_RDONLY));
    if (!fd)
      throw sys_error_t("opening NAR file '%s'", nar_path);
    auto source = fd_source_t{fd.get()};
    list(make_lazy_nar_accessor(source, seekable_get_nar_bytes(fd.get())), canon_path_t{path});
  }
};

static auto r_cmd_ls_store = registerCommand2<cmd_ls_store_t>({"store", "ls"});
static auto r_cmd_ls_nar = registerCommand2<cmd_ls_nar_t>({"nar", "ls"});
