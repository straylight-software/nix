#include <algorithm>
#include <array>
#include <sstream>

#include <nlohmann/json.hpp>

#include "nix/cmd/command.h"
#include "nix/main/common-args.h"
#include "nix/main/shared.h"
#include "nix/store/nar-info.h"
#include "nix/store/store-api.h"
#include "nix/util/strings.h"

/**
 * @return the total size of a set of store objects (specified by path),
 * that is, the sum of the size of the NAR serialisation of each object
 * in the set.
 */
static uint64_t get_store_objects_total_size(nix::store_t& store,
                                             const nix::store_path_set_t& closure) {
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
static nlohmann::json path_info_to_json(nix::store_t& store,
                                        const nix::store_path_set_t& store_paths,
                                        bool show_closure_size, nix::PathInfoJsonFormat format) {
  nlohmann::json::object_t json_all_objects = nlohmann::json::object();

  auto make_key = [&](const nix::store_path_t& path) {
    return format == nix::PathInfoJsonFormat::V1 ? store.printStorePath(path)
                                                 : std::string(path.to_string());
  };

  for (auto& store_path : store_paths) {
    nlohmann::json json_object;

    std::string key = make_key(store_path);

    try {
      auto info = store.queryPathInfo(store_path);

      // `storePath` has the representation `<hash>-x` rather than
      // `<hash>-<name>` in case of binary-cache stores & `--all` because we don't
      // know the name yet until we've read the NAR info.
      key = make_key(info->path);

      json_object =
          info->to_json(format == nix::PathInfoJsonFormat::V1 ? &store : nullptr, true, format);

      /* Hack in the store dir for now. TODO update the data type
         instead. */
      json_object["storeDir"] = store.store_dir;

      if (show_closure_size) {
        nix::store_path_set_t closure;
        store.computeFSClosure(store_path, closure, false, false);

        json_object["closureSize"] = get_store_objects_total_size(store, closure);

        if (dynamic_cast<const nix::nar_info_t*>(&*info)) {
          uint64_t totalDownloadSize = 0;
          for (auto& p : closure) {
            auto depInfo = store.queryPathInfo(p);
            if (auto* depNarInfo = dynamic_cast<const nix::nar_info_t*>(&*depInfo)) {
              totalDownloadSize += depNarInfo->file_size;
            } else {
              throw nix::Error("Missing .narinfo for dep %s of %s", store.printStorePath(p),
                               store.printStorePath(store_path));
            }
          }
          json_object["closureDownloadSize"] = totalDownloadSize;
        }
      }
    } catch (nix::InvalidPath&) {
      json_object = nullptr;
    }

    json_all_objects[key] = std::move(json_object);
  }

  if (format == nix::PathInfoJsonFormat::V1) {
    return json_all_objects;
  } else {
    return {
        {"version", format},
        {"storeDir", store.store_dir},
        {"info", std::move(json_all_objects)},
    };
  }
}

struct cmd_path_info_t : nix::StorePathsCommand, nix::MixJSON {
  bool show_size = false;
  bool show_closure_size = false;
  bool human_readable = false;
  bool show_sigs = false;
  std::optional<nix::PathInfoJsonFormat> json_format;

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
          json_format =
              nix::parse_path_info_json_format(nix::string2_int_with_unit_prefix<uint64_t>(s));
        }},
    });
  }

  std::string description() override { return "query information about store paths"; }

  std::string doc() override {
    return
#include "path-info.md"
        ;
  }

  category_t category() override { return nix::catSecondary; }

  void print_size(std::ostream& str, uint64_t value) {
    if (human_readable) {
      str << nix::fmt("\t%s", nix::render_size((int64_t)value, true));
    } else {
      str << nix::fmt("\t%11d", value);
    }
  }

  void run(nix::ref<nix::store_t> store, nix::store_paths_t&& store_paths) override {
    size_t path_len = 0;
    for (auto& store_path : store_paths) {
      path_len = std::max(path_len, store->printStorePath(store_path).size());
    }

    if (json) {
      printJSON(path_info_to_json(
          *store,
          // FIXME: preserve order?
          nix::store_path_set_t(store_paths.begin(), store_paths.end()), show_closure_size,
          json_format
              .or_else([&]() {
                nix::warn(
                    "'--json' without '--json-format' is deprecated; please specify '--json-format "
                    "1' or '--json-format 2'. This will become an error in a future release.");
                return std::optional{nix::PathInfoJsonFormat::V1};
              })
              .value()));
    }

    else {
      for (auto& store_path : store_paths) {
        auto info = store->queryPathInfo(store_path);
        auto store_path_s = store->printStorePath(info->path);

        std::string result = store_path_s;

        if (show_size || show_closure_size || show_sigs) {
          result += std::string(std::max(0, (int)path_len - (int)store_path_s.size()), ' ');
        }

        if (show_size) {
          std::ostringstream sink;
          print_size(sink, info->nar_size);
          result += sink.str();
        }

        if (show_closure_size) {
          nix::store_path_set_t closure;
          store->computeFSClosure(store_path, closure, false, false);
          std::ostringstream sink;
          print_size(sink, get_store_objects_total_size(*store, closure));
          result += sink.str();
        }

        if (show_sigs) {
          result += '\t';
          nix::strings_t ss;
          if (info->ultimate) {
            ss.push_back("ultimate");
          }
          if (info->ca) {
            ss.push_back("ca:" + nix::render_content_address(*info->ca));
          }
          for (auto& sig : info->sigs) {
            ss.push_back(sig);
          }
          result += nix::concat_strings_sep(" ", ss);
        }

        nix::logger->cout(result);
      }
    }
  }
};

static auto r_cmd_path_info = nix::registerCommand<cmd_path_info_t>("path-info");
