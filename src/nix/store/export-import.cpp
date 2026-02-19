#include "nix/store/export-import.h"

#include "nix/store/common-protocol-impl.h"
#include "nix/store/common-protocol.h"
#include "nix/store/store-api.h"
#include "nix/store/worker-protocol.h"
#include "nix/util/archive.h"
#include "nix/util/serialise.h"

namespace nix {

static const uint32_t export_magic_v1 = 0x4558494e;
static const uint64_t export_magic_v2 = 0x324f4952414e; // = 'NARIO2'

void export_paths(Store& store, const StorePathSet& paths, Sink& sink, unsigned int version) {
  auto sorted = store.topoSortPaths(paths);
  std::reverse(sorted.begin(), sorted.end());

  auto dump_nar = [&](const ValidPathInfo& info) {
    hash_sink_t hash_sink(hash_algorithm_t::SHA256);
    tee_sink_t tee_sink(sink, hash_sink);

    store.nar_from_path(info.path, tee_sink);

    /* Refuse to export paths that have changed.  This prevents
       filesystem corruption from spreading to other machines.
       Don't complain if the stored hash is zero (unknown). */
    Hash hash = hash_sink.current_hash().hash;
    if (hash != info.nar_hash && info.nar_hash != Hash(info.nar_hash.algo()))
      throw Error("hash of path '%s' has changed from '%s' to '%s'!",
                  store.printStorePath(info.path), info.nar_hash.to_string(hash_format_t::nix32, true),
                  hash.to_string(hash_format_t::nix32, true));
  };

  switch (version) {
    case 1:
      for (auto& path : sorted) {
        sink << 1;
        auto info = store.queryPathInfo(path);
        dump_nar(*info);
        sink << export_magic_v1 << store.printStorePath(path);
        CommonProto::write(store, CommonProto::WriteConn{.to = sink}, info->references);
        sink << (info->deriver ? store.printStorePath(*info->deriver) : "") << 0;
      }
      sink << 0;
      break;

    case 2:
      sink << export_magic_v2;

      for (auto& path : sorted) {
        activity_t act(*logger, lvl_talkative, act_unknown,
                     fmt("exporting path '%s'", store.printStorePath(path)));
        sink << 1;
        auto info = store.queryPathInfo(path);
        // FIXME: move to CommonProto?
        WorkerProto::Serialise<ValidPathInfo>::write(
            store, WorkerProto::WriteConn{.to = sink, .version = 16, .shortStorePaths = true},
            *info);
        dump_nar(*info);
      }

      sink << 0;
      break;

    default:
      throw Error("unsupported nario version %d", version);
  }
}

StorePaths import_paths(Store& store, Source& source, CheckSigsFlag check_sigs) {
  StorePaths res;

  auto version = read_num<uint64_t>(source);

  /* Note: nario version 1 lacks an explicit header. The first
     integer denotes whether a store path follows or not. So look
     for 0 or 1. */
  switch (version) {
    case 0:
      /* Empty version 1 nario, nothing to do. */
      break;

    case 1: {
      /* Reuse a string buffer to avoid kernel overhead allocating
         memory for large strings. */
      string_sink_t saved;

      /* Non-empty version 1 nario. */
      while (true) {
        /* Extract the NAR from the source. */
        saved.str().clear();
        tee_source_t tee{source, saved};
        null_file_system_object_sink_t ether;
        parse_dump(ether, tee);

        uint32_t magic = read_int(source);
        if (magic != export_magic_v1)
          throw Error("nario cannot be imported; wrong format");

        auto path = store.parseStorePath(read_string(source));

        auto references = CommonProto::Serialise<StorePathSet>::read(
            store, CommonProto::ReadConn{.from = source});
        auto deriver = read_string(source);

        // Ignore optional legacy signature.
        if (read_int(source) == 1)
          read_string(source);

        if (!store.isValidPath(path)) {
          auto nar_hash = hash_string(hash_algorithm_t::SHA256, saved.str());

          ValidPathInfo info{path, {store, nar_hash}};
          if (deriver != "")
            info.deriver = store.parseStorePath(deriver);
          info.references = references;
          info.nar_size = saved.str().size();

          // Can't use underlying source, which would have been exhausted.
          auto source2 = string_source_t(saved.str());
          store.add_to_store(info, source2, NoRepair, check_sigs);
        }

        res.push_back(path);

        auto n = read_num<uint64_t>(source);
        if (n == 0)
          break;
        if (n != 1)
          throw Error("input doesn't look like a nario");
      }
      break;
    }

    case export_magic_v2:
      while (true) {
        auto n = read_num<uint64_t>(source);
        if (n == 0)
          break;
        if (n != 1)
          throw Error("input doesn't look like a nario");

        auto info = WorkerProto::Serialise<ValidPathInfo>::read(
            store, WorkerProto::ReadConn{.from = source, .version = 16, .shortStorePaths = true});

        if (!store.isValidPath(info.path)) {
          activity_t act(*logger, lvl_talkative, act_unknown,
                       fmt("importing path '%s'", store.printStorePath(info.path)));

          store.add_to_store(info, source, NoRepair, check_sigs);
        } else
          source.skip(info.nar_size);

        res.push_back(info.path);
      }

      break;

    default:
      throw Error("input doesn't look like a nario");
  }

  return res;
}

} // namespace nix
