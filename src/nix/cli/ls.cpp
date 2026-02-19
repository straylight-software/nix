#include "ls.h"

#include <nlohmann/json.hpp>

#include "nix/cmd/command.h"
#include "nix/main/common-args.h"
#include "nix/store/store-api.h"
#include "nix/util/nar-accessor.h"

using namespace nix;

struct mix_ls_t : virtual Args, MixJSON, mix_long_listing_t {
  bool recursive = false;
  bool showDirectory = false;

  mix_ls_t() {
    addFlag({
        .longName = "recursive",
        .shortName = 'R',
        .description = "List subdirectories recursively.",
        .handler = {&recursive, true},
    });

    addFlag({
        .longName = "directory",
        .shortName = 'd',
        .description = "Show directories rather than their contents.",
        .handler = {&showDirectory, true},
    });
  }

  void listText(ref<SourceAccessor> accessor, canon_path_t path) {
    std::function<void(const SourceAccessor::stat_t&, const canon_path_t&, std::string_view, bool)>
        doPath;

    auto showFile = [&](const canon_path_t& curPath, std::string_view relPath) {
      if (longListing) {
        auto st = accessor->lstat(curPath);
        std::string tp = st.type == SourceAccessor::Type::tRegular
                             ? (st.isExecutable ? "-r-xr-xr-x" : "-r--r--r--")
                         : st.type == SourceAccessor::Type::tSymlink ? "lrwxrwxrwx"
                                                                     : "dr-xr-xr-x";
        auto line = fmt("%s %9d %s", tp, st.fileSize.value_or(0), relPath);
        if (st.type == SourceAccessor::Type::tSymlink)
          line += " -> " + accessor->readLink(curPath);
        logger->cout(line);
        if (recursive && st.type == SourceAccessor::Type::tDirectory)
          doPath(st, curPath, relPath, false);
      } else {
        logger->cout(relPath);
        if (recursive) {
          auto st = accessor->lstat(curPath);
          if (st.type == SourceAccessor::Type::tDirectory)
            doPath(st, curPath, relPath, false);
        }
      }
    };

    doPath = [&](const SourceAccessor::stat_t& st, const canon_path_t& curPath, std::string_view relPath,
                 bool showDirectory) {
      if (st.type == SourceAccessor::Type::tDirectory && !showDirectory) {
        auto names = accessor->readDirectory(curPath);
        for (auto& [name, type] : names)
          showFile(curPath / name, relPath + "/" + name);
      } else
        showFile(curPath, relPath);
    };

    auto st = accessor->lstat(path);
    doPath(st, path,
           st.type == SourceAccessor::Type::tDirectory ? "." : path.baseName().value_or(""),
           showDirectory);
  }

  void list(ref<SourceAccessor> accessor, canon_path_t path) {
    if (json) {
      if (showDirectory)
        throw UsageError("'--directory' is useless with '--json'");
      nlohmann::json j;
      if (recursive)
        j = listNarDeep(*accessor, path);
      else
        j = listNarShallow(*accessor, path);
      logger->cout("%s", j.dump());
    } else
      listText(accessor, std::move(path));
  }
};

struct cmd_ls_store_t : StoreCommand, mix_ls_t {
  std::string path;

  cmd_ls_store_t() { expectArgs({.label = "path", .handler = {&path}, .completer = completePath}); }

  std::string description() override { return "show information about a path in the Nix store"; }

  std::string doc() override {
    return
#include "store-ls.md"
        ;
  }

  void run(ref<Store> store) override {
    auto [storePath, rest] = store->toStorePath(path);
    list(store->requireStoreObjectAccessor(storePath), canon_path_t{rest});
  }
};

struct cmd_ls_nar_t : command_t, mix_ls_t {
  Path narPath;

  std::string path;

  cmd_ls_nar_t() {
    expectArgs({.label = "nar", .handler = {&narPath}, .completer = completePath});
    expectArg("path", &path);
  }

  std::string doc() override {
    return
#include "nar-ls.md"
        ;
  }

  std::string description() override { return "show information about a path inside a NAR file"; }

  void run() override {
    auto_close_fd_t fd = toDescriptor(open(narPath.c_str(), O_RDONLY));
    if (!fd)
      throw sys_error_t("opening NAR file '%s'", narPath);
    auto source = fd_source_t{fd.get()};
    list(makeLazyNarAccessor(source, seekableGetNarBytes(fd.get())), canon_path_t{path});
  }
};

static auto rCmdLsStore = registerCommand2<cmd_ls_store_t>({"store", "ls"});
static auto rCmdLsNar = registerCommand2<cmd_ls_nar_t>({"nar", "ls"});
