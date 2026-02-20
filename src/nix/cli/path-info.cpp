#include <algorithm>
#include <array>

#include <nlohmann/json.hpp>

#include "nix/cmd/command.h"
#include "nix/main/common-args.h"
#include "nix/main/shared.h"
#include "nix/store/nar-info.h"
#include "nix/store/store-api.h"
#include "nix/util/strings.h"

using namespace nix;
using nlohmann::json;

/**
 * @return the total size of a set of store objects (specified by path),
 * that is, the sum of the size of the NAR serialisation of each object
 * in the set.
 */
static uint64_t get_store_objects_total_size(store_t& store, const store_path_set_t& closure) {
  uint64_t total_nar_size = 0;
  for (auto& p : closure) {
    total_nar_size += store.queryPathInfo(p)->nar_size;
  }
  return total_nar_size;
}

/**
 * Write a JSON representation of store object metadata, such as the
 * hash and the references.
 *
 * @param show_closure_size If true, the closure size of each path is
 * included.
 * @param format The JSON format version to use.
 */
static json path_info_to_json(store_t& store, const store_path_set_t& store_paths,
                              bool show_closure_size, PathInfoJsonFormat format) {
  json::object_t json_all_objects = json::object();

  auto make_key = [&](const store_path_t& path) {
    return format == PathInfoJsonFormat::V1 ? store.printStorePath(path)
                                            : std::string(path.to_string());
  };

  for (auto& store_path : store_paths) {
    json json_object;

    std::string key = make_key(store_path);

    try {
      auto info = store.queryPathInfo(store_path);

      // `storePath` has the representation `<hash>-x` rather than
      // `<hash>-<name>` in case of binary-cache stores & `--all` because we don't
      // know the name yet until we've read the NAR info.
      key = make_key(info->path);

      json_object =
          info->to_json(format == PathInfoJsonFormat::V1 ? &store : nullptr, true, format);

      /* Hack in the store dir for now. TODO update the data type
         instead. */
      json_object["storeDir"] = store.store_dir;

      if (show_closure_size) {
        store_path_set_t closure;
        store.computeFSClosure(store_path, closure, false, false);

        json_object["closureSize"] = get_store_objects_total_size(store, closure);

        if (dynamic_cast<const nar_info_t*>(&*info)) {
          uint64_t totalDownloadSize = 0;
          for (auto& p : closure) {
            auto depInfo = store.queryPathInfo(p);
            if (auto* depNarInfo = dynamic_cast<const nar_info_t*>(&*depInfo))
              totalDownloadSize += depNarInfo->file_size;
            else
              throw Error("Missing .narinfo for dep %s of %s", store.printStorePath(p),
                          store.printStorePath(store_path));
          }
          json_object["closureDownloadSize"] = totalDownloadSize;
        }
      }
    } catch (InvalidPath&) {
      json_object = nullptr;
    }

    json_all_objects[key] = std::move(json_object);
  }

  if (format == PathInfoJsonFormat::V1) {
    return json_all_objects;
  } else {
    return {
        {"version", format},
        {"storeDir", store.store_dir},
        {"info", std::move(json_all_objects)},
    };
  }
}

struct cmd_path_info_t : StorePathsCommand, MixJSON {
  bool show_size = false;
  bool show_closure_size = false;
  bool human_readable = false;
  bool show_sigs = false;
  std::optional<PathInfoJsonFormat> json_format;

  cmd_path_info_t() {
    add_flag({
        .long_name = "size",
        .short_name = 's',
        .description = "Print the size of the NAR serialisation of each path.",
        .handler = {&show_size, true},
    });

    add_flag({
        .long_name = "closure-size",
        .short_name = 'S',
        .description =
            "Print the sum of the sizes of the NAR serialisations of the closure of each path.",
        .handler = {&show_closure_size, true},
    });

    add_flag({
        .long_name = "human-readable",
        .short_name = 'h',
        .description =
            "With `-s` and `-S`, print sizes in a human-friendly format such as `5.67G`.",
        .handler = {&human_readable, true},
    });

    add_flag({
        .long_name = "sigs",
        .description = "Show signatures.",
        .handler = {&show_sigs, true},
    });

    add_flag({
        .long_name = "json-format",
        .description = "JSON format version to use (1 or 2). Version 1 uses string hashes and full "
                       "store paths. Version 2 uses structured hashes and store path base names. "
                       "This flag will be required in a future release.",
        .labels = {"version"},
        .handler = {[this](std::string s) {
          json_format = parse_path_info_json_format(string2_int_with_unit_prefix<uint64_t>(s));
        }},
    });
  }

  std::string description() override { return "query information about store paths"; }

  std::string doc() override {
    return
#include "path-info.md"
        ;
  }

  category_t category() override { return catSecondary; }

  void print_size(std::ostream& str, uint64_t value) {
    if (human_readable)
      str << fmt("\t%s", render_size((int64_t)value, true));
    else
      str << fmt("\t%11d", value);
  }

  void run(ref<store_t> store, store_paths_t&& store_paths) override {
    size_t path_len = 0;
    for (auto& store_path : store_paths)
      path_len = std::max(path_len, store->printStorePath(store_path).size());

    if (json) {
      printJSON(path_info_to_json(
          *store,
          // FIXME: preserve order?
          store_path_set_t(store_paths.begin(), store_paths.end()), show_closure_size,
          json_format
              .or_else([&]() {
                warn(
                    "'--json' without '--json-format' is deprecated; please specify '--json-format "
                    "1' or '--json-format 2'. This will become an error in a future release.");
                return std::optional{PathInfoJsonFormat::V1};
              })
              .value()));
    }

    else {
      for (auto& store_path : store_paths) {
        auto info = store->queryPathInfo(store_path);
        auto store_path_s = store->printStorePath(info->path);

        std::ostringstream str;

        str << store_path_s;

        if (show_size || show_closure_size || show_sigs)
          str << std::string(std::max(0, (int)path_len - (int)store_path_s.size()), ' ');

        if (show_size)
          print_size(str, info->nar_size);

        if (show_closure_size) {
          store_path_set_t closure;
          store->computeFSClosure(store_path, closure, false, false);
          print_size(str, get_store_objects_total_size(*store, closure));
        }

        if (show_sigs) {
          str << '\t';
          strings_t ss;
          if (info->ultimate)
            ss.push_back("ultimate");
          if (info->ca)
            ss.push_back("ca:" + render_content_address(*info->ca));
          for (auto& sig : info->sigs)
            ss.push_back(sig);
          str << concat_strings_sep(" ", ss);
        }

        logger->cout(str.str());
      }
    }
  }
};

static auto r_cmd_path_info = registerCommand<cmd_path_info_t>("path-info");
