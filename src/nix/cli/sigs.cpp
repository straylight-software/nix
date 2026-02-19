#include <atomic>

#include "nix/cmd/command.h"
#include "nix/main/shared.h"
#include "nix/store/filetransfer.h"
#include "nix/store/store-open.h"
#include "nix/util/signals.h"
#include "nix/util/thread-pool.h"

using namespace nix;

struct cmd_copy_sigs_t : StorePathsCommand {
  strings_t substituterUris;

  cmd_copy_sigs_t() {
    addFlag({
        .longName = "substituter",
        .shortName = 's',
        .description = "Copy signatures from the specified store.",
        .labels = {"store-uri"},
        .handler = {[&](std::string s) { substituterUris.push_back(s); }},
    });
  }

  std::string description() override { return "copy store path signatures from substituters"; }

  std::string doc() override {
    return
#include "store-copy-sigs.md"
        ;
  }

  void run(ref<Store> store, StorePaths&& storePaths) override {
    if (substituterUris.empty())
      throw UsageError("you must specify at least one substituter using '-s'");

    // FIXME: factor out commonality with MixVerify.
    std::vector<ref<Store>> substituters;
    for (auto& s : substituterUris)
      substituters.push_back(openStore(s));

    thread_pool_t pool{fileTransferSettings.httpConnections};

    std::atomic<size_t> added{0};

    // logger->setExpected(doneLabel, storePaths.size());

    auto doPath = [&](const Path& storePathS) {
      // Activity act(*logger, lvlInfo, "getting signatures for '%s'", storePath);

      checkInterrupt();

      auto storePath = store->parseStorePath(storePathS);

      auto info = store->queryPathInfo(storePath);

      string_set_t newSigs;

      for (auto& store2 : substituters) {
        try {
          auto info2 = store2->queryPathInfo(info->path);

          /* Don't import signatures that don't match this
             binary. */
          if (info->narHash != info2->narHash || info->narSize != info2->narSize ||
              info->references != info2->references)
            continue;

          for (auto& sig : info2->sigs)
            if (!info->sigs.count(sig))
              newSigs.insert(sig);
        } catch (InvalidPath&) {
        }
      }

      if (!newSigs.empty()) {
        store->addSignatures(storePath, newSigs);
        added += newSigs.size();
      }

      // logger->incProgress(doneLabel);
    };

    for (auto& storePath : storePaths)
      pool.enqueue(std::bind(doPath, store->printStorePath(storePath)));

    pool.process();

    printInfo("imported %d signatures", added);
  }
};

static auto rCmdCopySigs = registerCommand2<cmd_copy_sigs_t>({"store", "copy-sigs"});

struct cmd_sign_t : StorePathsCommand {
  Path secretKeyFile;

  cmd_sign_t() {
    addFlag({
        .longName = "key-file",
        .shortName = 'k',
        .description = "File containing the secret signing key.",
        .labels = {"file"},
        .handler = {&secretKeyFile},
        .completer = completePath,
        .required = true,
    });
  }

  std::string description() override { return "sign store paths with a local key"; }

  void run(ref<Store> store, StorePaths&& storePaths) override {
    secret_key_t secretKey(readFile(secretKeyFile));
    local_signer_t signer(std::move(secretKey));

    size_t added{0};

    for (auto& storePath : storePaths) {
      auto info = store->queryPathInfo(storePath);

      auto info2(*info);
      info2.sigs.clear();
      info2.sign(*store, signer);
      assert(!info2.sigs.empty());

      if (!info->sigs.count(*info2.sigs.begin())) {
        store->addSignatures(storePath, info2.sigs);
        added++;
      }
    }

    printInfo("added %d signatures", added);
  }
};

static auto rCmdSign = registerCommand2<cmd_sign_t>({"store", "sign"});

struct cmd_key_generate_secret_t : command_t {
  std::string keyName;

  cmd_key_generate_secret_t() {
    addFlag({
        .longName = "key-name",
        .description = "Identifier of the key (e.g. `cache.example.org-1`).",
        .labels = {"name"},
        .handler = {&keyName},
        .required = true,
    });
  }

  std::string description() override { return "generate a secret key for signing store paths"; }

  std::string doc() override {
    return
#include "key-generate-secret.md"
        ;
  }

  void run() override {
    logger->stop();
    writeFull(getStandardOutput(), secret_key_t::generate(keyName).to_string());
  }
};

struct cmd_key_convert_secret_to_public_t : command_t {
  std::string description() override {
    return "generate a public key for verifying store paths from a secret key read from standard "
           "input";
  }

  std::string doc() override {
    return
#include "key-convert-secret-to-public.md"
        ;
  }

  void run() override {
    secret_key_t secretKey(drainFD(STDIN_FILENO));
    logger->stop();
    writeFull(getStandardOutput(), secretKey.toPublicKey().to_string());
  }
};

struct cmd_key_t : NixMultiCommand {
  cmd_key_t()
      : NixMultiCommand("key",
                        {
                            {"generate-secret", []() { return make_ref<cmd_key_generate_secret_t>(); }},
                            {"convert-secret-to-public",
                             []() { return make_ref<cmd_key_convert_secret_to_public_t>(); }},
                        }) {}

  std::string description() override { return "generate and convert Nix signing keys"; }

  category_t category() override { return catUtility; }
};

static auto rCmdKey = registerCommand<cmd_key_t>("key");
