#include <atomic>

#include "nix/cmd/command.h"
#include "nix/main/shared.h"
#include "nix/store/keys.h"
#include "nix/store/store-open.h"
#include "nix/util/exit.h"
#include "nix/util/signals.h"
#include "nix/util/thread-pool.h"

struct cmd_verify_t : nix::StorePathsCommand {
  bool no_contents = false;
  bool no_trust = false;
  nix::strings_t substituter_uris;
  size_t sigs_needed = 0;

  cmd_verify_t() {
    add_flag({
        .long_name = "no-contents",
        .description = "Do not verify the contents of each store path.",
        .handler = {&no_contents, true},
    });

    add_flag({
        .long_name = "no-trust",
        .description = "Do not verify whether each store path is trusted.",
        .handler = {&no_trust, true},
    });

    add_flag({
        .long_name = "substituter",
        .short_name = 's',
        .description = "Use signatures from the specified store.",
        .labels = {"store-uri"},
        .handler = {[&](std::string s) { substituter_uris.push_back(s); }},
    });

    add_flag({
        .long_name = "sigs-needed",
        .short_name = 'n',
        .description = "Require that each path is signed by at least *n* different keys.",
        .labels = {"n"},
        .handler = {&sigs_needed},
    });
  }

  std::string description() override { return "verify the integrity of store paths"; }

  std::string doc() override {
    return
#include "verify.md"
        ;
  }

  void run(nix::ref<nix::store_t> store, nix::store_paths_t&& store_paths) override {
    std::vector<nix::ref<nix::store_t>> substituters;
    for (auto& s : substituter_uris)
      substituters.push_back(nix::open_store(s));

    auto public_keys = nix::get_default_public_keys();

    nix::activity_t act(*nix::logger, nix::act_verify_paths);

    std::atomic<size_t> done{0};
    std::atomic<size_t> untrusted{0};
    std::atomic<size_t> corrupted{0};
    std::atomic<size_t> failed{0};
    std::atomic<size_t> active{0};

    auto update = [&]() { act.progress(done, store_paths.size(), active, failed); };

    nix::thread_pool_t pool;

    auto do_path = [&](const nix::store_path_t& store_path) {
      try {
        nix::check_interrupt();

        nix::maintain_count_t<std::atomic<size_t>> mcActive(active);
        update();

        auto info = store->queryPathInfo(store_path);

        // Note: info->path can be different from storePath
        // for binary cache stores when using --all (since we
        // can't enumerate names efficiently).
        nix::activity_t act2(*nix::logger, nix::lvl_info, nix::act_unknown,
                             nix::fmt("checking '%s'", store->printStorePath(info->path)));

        if (!no_contents) {
          auto hash_sink = nix::hash_sink_t(info->nar_hash.algo());

          store->nar_from_path(info->path, hash_sink);

          auto hash = hash_sink.finish();

          if (hash.hash != info->nar_hash) {
            corrupted++;
            act2.result(nix::res_corrupted_path, store->printStorePath(info->path));
            nix::printError("path '%s' was modified! expected hash '%s', got '%s'",
                            store->printStorePath(info->path),
                            info->nar_hash.to_string(nix::hash_format_t::nix32, true),
                            hash.hash.to_string(nix::hash_format_t::nix32, true));
          }
        }

        if (!no_trust) {
          bool good = false;

          if (info->ultimate && !sigs_needed)
            good = true;

          else {
            nix::string_set_t sigs_seen;
            size_t actual_sigs_needed = std::max(sigs_needed, (size_t)1);
            size_t valid_sigs = 0;

            auto do_sigs = [&](nix::string_set_t sigs) {
              for (const auto& sig : sigs) {
                if (!sigs_seen.insert(sig).second)
                  continue;
                if (valid_sigs < nix::valid_path_info_t::maxSigs &&
                    info->checkSignature(*store, public_keys, sig))
                  valid_sigs++;
              }
            };

            if (info->isContentAddressed(*store))
              valid_sigs = nix::valid_path_info_t::maxSigs;

            do_sigs(info->sigs);

            for (auto& store2 : substituters) {
              if (valid_sigs >= actual_sigs_needed)
                break;
              try {
                auto info2 = store2->queryPathInfo(info->path);
                if (info2->isContentAddressed(*store))
                  valid_sigs = nix::valid_path_info_t::maxSigs;
                do_sigs(info2->sigs);
              } catch (nix::InvalidPath&) {
              } catch (nix::Error& e) {
                nix::logError(e.info());
              }
            }

            if (valid_sigs >= actual_sigs_needed)
              good = true;
          }

          if (!good) {
            untrusted++;
            act2.result(nix::res_untrusted_path, store->printStorePath(info->path));
            nix::printError("path '%s' is untrusted", store->printStorePath(info->path));
          }
        }

        done++;

      } catch (nix::Error& e) {
        nix::logError(e.info());
        failed++;
      }

      update();
    };

    for (auto& store_path : store_paths)
      pool.enqueue(std::bind(do_path, store_path));

    pool.process();

    throw nix::exit_t((corrupted ? 1 : 0) | (untrusted ? 2 : 0) | (failed ? 4 : 0));
  }
};

static auto r_cmd_verify = nix::registerCommand2<cmd_verify_t>({"store", "verify"});
