#include <fcntl.h>

#include <nlohmann/json.hpp>

#include "man-pages.h"
#include "nix/cmd/command.h"
#include "nix/cmd/legacy.h"
#include "nix/cmd/misc-store-flags.h"
#include "nix/expr/attr-path.h"
#include "nix/expr/eval-inline.h"
#include "nix/main/common-args.h"
#include "nix/main/loggers.h"
#include "nix/main/shared.h"
#include "nix/store/filetransfer.h"
#include "nix/store/path.h"
#include "nix/store/store-open.h"
#include "nix/util/environment-variables.h"
#include "nix/util/finally.h"
#include "nix/util/posix-source-accessor.h"
#include "nix/util/tarfile.h"
#include "nix/util/terminal.h"
#include "nix/util/url.h"

using namespace nix;

/* If ‘url’ starts with ‘mirror://’, then resolve it using the list of
   mirrors defined in Nixpkgs. */
std::string resolve_mirror_url(eval_state_t& state, const std::string& url) {
  if (url.substr(0, 9) != "mirror://")
    return url;

  std::string s(url, 9);
  auto p = s.find('/');
  if (p == std::string::npos)
    throw Error("invalid mirror URL '%s'", url);
  std::string mirrorName(s, 0, p);

  value_t v_mirrors;
  // FIXME: use nixpkgs flake
  state.eval(state.parseExprFromString("import <nixpkgs/pkgs/build-support/fetchurl/mirrors.nix>",
                                       state.root_path(canon_path_t::root)),
             v_mirrors);
  state.forceAttrs(v_mirrors, no_pos, "while evaluating the set of all mirrors");

  auto mirror_list = v_mirrors.attrs()->get(state.symbols.create(mirrorName));
  if (!mirror_list)
    throw Error("unknown mirror name '%s'", mirrorName);
  state.forceList(*mirror_list->value, no_pos, "while evaluating one mirror configuration");

  if (mirror_list->value->list_size() < 1)
    throw Error("mirror URL '%s' did not expand to anything", url);

  std::string mirror(state.forceString(*mirror_list->value->list_view()[0], no_pos,
                                       "while evaluating the first available mirror"));
  return mirror + (has_suffix(mirror, "/") ? "" : "/") + s.substr(p + 1);
}

std::tuple<store_path_t, Hash> prefetch_file(ref<store_t> store, const verbatim_url_t& url,
                                          std::optional<std::string> maybe_name,
                                          hash_algorithm_t hash_algo,
                                          std::optional<Hash> expected_hash, bool unpack,
                                          bool executable) {
  content_address_method_t method = unpack || executable ? content_address_method_t::raw_t::nix_archive
                                                     : content_address_method_t::raw_t::flat;

  std::string name = maybe_name
                         .or_else([&]() {
                           /* Figure out a name in the Nix store. */
                           auto derived_from_url = url.last_path_segment();
                           if (!derived_from_url || derived_from_url->empty())
                             throw Error("cannot figure out file name for '%s'", url.to_string());
                           return derived_from_url;
                         })
                         .value();

  try {
    check_name(name);
  } catch (BadStorePathName& e) {
    if (!maybe_name)
      e.add_trace({}, "file name '%s' was extracted from URL '%s'", name, url.to_string());
    throw;
  }

  std::optional<store_path_t> store_path;
  std::optional<Hash> hash;

  /* If an expected hash is given, the file may already exist in
     the store. */
  if (expected_hash) {
    hash_algo = expected_hash->algo();
    store_path = store->makeFixedOutputPathFromCA(
        name, ContentAddressWithReferences::fromParts(method, *expected_hash, {}));
    if (store->isValidPath(*store_path))
      hash = expected_hash;
    else
      store_path.reset();
  }

  if (!store_path) {
    auto_delete_t tmp_dir(create_temp_dir(), true);
    std::filesystem::path tmp_file = tmp_dir.path() / "tmp";

    /* Download the file. */
    {
      auto mode = 0600;
      if (executable)
        mode = 0700;

      auto_close_fd_t fd =
          to_descriptor(open(tmp_file.string().c_str(), O_WRONLY | O_CREAT | O_EXCL, mode));
      if (!fd)
        throw sys_error_t("creating temporary file '%s'", tmp_file);

      fd_sink_t sink(fd.get());

      FileTransferRequest req(url);
      req.decompress = false;
      get_file_transfer()->download(std::move(req), sink);
    }

    /* Optionally unpack the file. */
    if (unpack) {
      activity_t act(*logger, lvl_chatty, act_unknown, fmt("unpacking '%s'", url.to_string()));
      auto unpacked = (tmp_dir.path() / "unpacked").string();
      create_dirs(unpacked);
      unpack_tarfile(tmp_file.string(), unpacked);

      auto entries = directory_iterator_t{unpacked};
      /* If the archive unpacks to a single file/directory, then use
         that as the top-level. */
      tmp_file = entries->path();
      auto file_count = std::distance(entries, directory_iterator_t{});
      if (file_count != 1) {
        /* otherwise, use the directory itself */
        tmp_file = unpacked;
      }
    }

    activity_t act(*logger, lvl_chatty, act_unknown,
                   fmt("adding '%s' to the store", url.to_string()));

    auto info = store->addToStoreSlow(name, make_fs_source_accessor(tmp_file), method, hash_algo,
                                      {}, expected_hash);
    store_path = info.path;
    assert(info.ca);
    hash = info.ca->hash;
  }

  return {store_path.value(), hash.value()};
}

static int main_nix_prefetch_url(int argc, char** argv) {
  {
    hash_algorithm_t ha = hash_algorithm_t::SHA256;
    std::vector<std::string> args;
    bool print_path = get_env("PRINT_PATH") == "1";
    bool from_expr = false;
    std::string attr_path;
    bool unpack = false;
    bool executable = false;
    std::optional<std::string> name;

    struct my_args_t : LegacyArgs, MixEvalArgs {
      using LegacyArgs::LegacyArgs;
    };

    my_args_t my_args(std::string(base_name_of(argv[0])),
                      [&](strings_t::iterator& arg, const strings_t::iterator& end) {
                        if (*arg == "--help")
                          show_man_page("nix-prefetch-url");
                        else if (*arg == "--version")
                          print_version("nix-prefetch-url");
                        else if (*arg == "--type") {
                          auto s = get_arg(*arg, arg, end);
                          ha = parse_hash_algo(s);
                        } else if (*arg == "--print-path")
                          print_path = true;
                        else if (*arg == "--attr" || *arg == "-A") {
                          from_expr = true;
                          attr_path = get_arg(*arg, arg, end);
                        } else if (*arg == "--unpack")
                          unpack = true;
                        else if (*arg == "--executable")
                          executable = true;
                        else if (*arg == "--name")
                          name = get_arg(*arg, arg, end);
                        else if (*arg != "" && arg->at(0) == '-')
                          return false;
                        else
                          args.push_back(*arg);
                        return true;
                      });

    my_args.parse_cmdline(argv_to_strings(argc, argv));

    if (args.size() > 2)
      throw UsageError("too many arguments");

    set_log_format("bar");

    auto store = open_store();
    auto state =
        std::make_unique<eval_state_t>(my_args.lookup_path, store, fetch_settings, eval_settings);

    bindings_t& auto_args = *my_args.getAutoArgs(*state);

    /* If -A is given, get the URL from the specified Nix
       expression. */
    std::string url;
    if (!from_expr) {
      if (args.empty())
        throw UsageError("you must specify a URL");
      url = args[0];
    } else {
      value_t v_root;
      state->evalFile(resolve_expr_path(lookup_file_arg(*state, args.empty() ? "." : args[0])),
                      v_root);
      value_t& v(*find_along_attr_path(*state, attr_path, auto_args, v_root).first);
      state->forceAttrs(v, no_pos, "while evaluating the source attribute to prefetch");

      /* Extract the URL. */
      auto* attr = v.attrs()->get(state->symbols.create("urls"));
      if (!attr)
        throw Error("attribute 'urls' missing");
      state->forceList(*attr->value, no_pos, "while evaluating the urls to prefetch");
      if (attr->value->list_size() < 1)
        throw Error("'urls' list is empty");
      url = state->forceString(*attr->value->list_view()[0], no_pos,
                               "while evaluating the first url from the urls list");

      /* Extract the hash mode. */
      auto attr2 = v.attrs()->get(state->symbols.create("outputHashMode"));
      if (!attr2)
        printInfo("warning: this does not look like a fetchurl call");
      else
        unpack =
            state->forceString(*attr2->value, no_pos,
                               "while evaluating the outputHashMode of the source to prefetch") ==
            "recursive";

      /* Extract the name. */
      if (!name) {
        auto attr3 = v.attrs()->get(state->symbols.create("name"));
        if (!attr3)
          name = state->forceString(*attr3->value, no_pos,
                                    "while evaluating the name of the source to prefetch");
      }
    }

    std::optional<Hash> expected_hash;
    if (args.size() == 2)
      expected_hash = Hash::parse_any(args[1], ha);

    auto [store_path, hash] = prefetch_file(store, resolve_mirror_url(*state, url), name, ha,
                                            expected_hash, unpack, executable);

    logger->stop();

    if (!print_path)
      printInfo("path is '%s'", store->printStorePath(store_path));

    assert(static_cast<char>(hash.algo()));
    logger->cout(hash.to_string(hash.algo() == hash_algorithm_t::MD5 ? hash_format_t::base16
                                                                     : hash_format_t::nix32,
                                false));

    if (print_path)
      logger->cout(store->printStorePath(store_path));

    return 0;
  }
}

static RegisterLegacyCommand r_nix_prefetch_url("nix-prefetch-url", main_nix_prefetch_url);

struct cmd_store_prefetch_file_t : StoreCommand, MixJSON {
  std::string url;
  bool executable = false;
  bool unpack = false;
  std::optional<std::string> name;
  hash_algorithm_t hash_algo = hash_algorithm_t::SHA256;
  std::optional<Hash> expected_hash;

  cmd_store_prefetch_file_t() {
    add_flag({
        .long_name = "name",
        .description = "Override the name component of the resulting store path. It defaults to "
                       "the base name of *url*.",
        .labels = {"name"},
        .handler = {&name},
    });

    add_flag({
        .long_name = "expected-hash",
        .description = "The expected hash of the file.",
        .labels = {"hash"},
        .handler = {[&](std::string s) { expected_hash = Hash::parse_any(s, hash_algo); }},
    });

    add_flag(flag::hash_algo("hash-type", &hash_algo));

    add_flag({
        .long_name = "executable",
        .description = "Make the resulting file executable. Note that this causes the "
                       "resulting hash to be a NAR hash rather than a flat file hash.",
        .handler = {&executable, true},
    });

    add_flag({
        .long_name = "unpack",
        .description = "Unpack the archive (which must be a tarball or zip file) and add "
                       "the result to the Nix store.",
        .handler = {&unpack, true},
    });

    expect_arg("url", &url);
  }

  std::string description() override { return "download a file into the Nix store"; }

  std::string doc() override {
    return
#include "store-prefetch-file.md"
        ;
  }

  void run(ref<store_t> store) override {
    auto [store_path, hash] =
        prefetch_file(store, url, name, hash_algo, expected_hash, unpack, executable);

    if (json) {
      auto res = nlohmann::json::object();
      res["storePath"] = store->printStorePath(store_path);
      res["hash"] = hash.to_string(hash_format_t::sri, true);
      printJSON(res);
    } else {
      notice("Downloaded '%s' to '%s' (hash '%s').", url, store->printStorePath(store_path),
             hash.to_string(hash_format_t::sri, true));
    }
  }
};

static auto r_cmd_store_prefetch_file =
    registerCommand2<cmd_store_prefetch_file_t>({"store", "prefetch-file"});
