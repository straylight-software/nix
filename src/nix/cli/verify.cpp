#include <atomic>

#include "nix/cmd/command.h"
#include "nix/main/shared.h"
#include "nix/store/keys.h"
#include "nix/store/store-open.h"
#include "nix/util/exit.h"
#include "nix/util/signals.h"
#include "nix/util/thread-pool.h"

using namespace nix;

struct cmd_verify_t : StorePathsCommand {
  bool noContents = false;
  bool noTrust = false;
  strings_t substituterUris;
  size_t sigsNeeded = 0;

  cmd_verify_t() {
    addFlag({
        .longName = "no-contents",
        .description = "Do not verify the contents of each store path.",
        .handler = {&noContents, true},
    });

    addFlag({
        .longName = "no-trust",
        .description = "Do not verify whether each store path is trusted.",
        .handler = {&noTrust, true},
    });

    addFlag({
        .longName = "substituter",
        .shortName = 's',
        .description = "Use signatures from the specified store.",
        .labels = {"store-uri"},
        .handler = {[&](std::string s) { substituterUris.push_back(s); }},
    });

    addFlag({
        .longName = "sigs-needed",
        .shortName = 'n',
        .description = "Require that each path is signed by at least *n* different keys.",
        .labels = {"n"},
        .handler = {&sigsNeeded},
    });
  }

  std::string description() override { return "verify the integrity of store paths"; }

  std::string doc() override {
    return
#include "verify.md"
        ;
  }

  void run(ref<Store> store, StorePaths&& storePaths) override {
    std::vector<ref<Store>> substituters;
    for (auto& s : substituterUris)
      substituters.push_back(openStore(s));

    auto publicKeys = getDefaultPublicKeys();

    activity_t act(*logger, actVerifyPaths);

    std::atomic<size_t> done{0};
    std::atomic<size_t> untrusted{0};
    std::atomic<size_t> corrupted{0};
    std::atomic<size_t> failed{0};
    std::atomic<size_t> active{0};

    auto update = [&]() { act.progress(done, storePaths.size(), active, failed); };

    thread_pool_t pool;

    auto doPath = [&](const StorePath& storePath) {
      try {
        checkInterrupt();

        maintain_count_t<std::atomic<size_t>> mcActive(active);
        update();

        auto info = store->queryPathInfo(storePath);

        // Note: info->path can be different from storePath
        // for binary cache stores when using --all (since we
        // can't enumerate names efficiently).
        activity_t act2(*logger, lvlInfo, actUnknown,
                      fmt("checking '%s'", store->printStorePath(info->path)));

        if (!noContents) {
          auto hashSink = hash_sink_t(info->narHash.algo);

          store->narFromPath(info->path, hashSink);

          auto hash = hashSink.finish();

          if (hash.hash != info->narHash) {
            corrupted++;
            act2.result(resCorruptedPath, store->printStorePath(info->path));
            printError("path '%s' was modified! expected hash '%s', got '%s'",
                       store->printStorePath(info->path),
                       info->narHash.to_string(hash_format_t::Nix32, true),
                       hash.hash.to_string(hash_format_t::Nix32, true));
          }
        }

        if (!noTrust) {
          bool good = false;

          if (info->ultimate && !sigsNeeded)
            good = true;

          else {
            string_set_t sigsSeen;
            size_t actualSigsNeeded = std::max(sigsNeeded, (size_t)1);
            size_t validSigs = 0;

            auto doSigs = [&](string_set_t sigs) {
              for (const auto& sig : sigs) {
                if (!sigsSeen.insert(sig).second)
                  continue;
                if (validSigs < ValidPathInfo::maxSigs &&
                    info->checkSignature(*store, publicKeys, sig))
                  validSigs++;
              }
            };

            if (info->isContentAddressed(*store))
              validSigs = ValidPathInfo::maxSigs;

            doSigs(info->sigs);

            for (auto& store2 : substituters) {
              if (validSigs >= actualSigsNeeded)
                break;
              try {
                auto info2 = store2->queryPathInfo(info->path);
                if (info2->isContentAddressed(*store))
                  validSigs = ValidPathInfo::maxSigs;
                doSigs(info2->sigs);
              } catch (InvalidPath&) {
              } catch (Error& e) {
                logError(e.info());
              }
            }

            if (validSigs >= actualSigsNeeded)
              good = true;
          }

          if (!good) {
            untrusted++;
            act2.result(resUntrustedPath, store->printStorePath(info->path));
            printError("path '%s' is untrusted", store->printStorePath(info->path));
          }
        }

        done++;

      } catch (Error& e) {
        logError(e.info());
        failed++;
      }

      update();
    };

    for (auto& storePath : storePaths)
      pool.enqueue(std::bind(doPath, storePath));

    pool.process();

    throw exit_t((corrupted ? 1 : 0) | (untrusted ? 2 : 0) | (failed ? 4 : 0));
  }
};

static auto rCmdVerify = registerCommand2<cmd_verify_t>({"store", "verify"});
