#include <atomic>

#include "nix/cmd/command.h"
#include "nix/main/shared.h"
#include "nix/store/filetransfer.h"
#include "nix/store/store-open.h"
#include "nix/util/signals.h"
#include "nix/util/thread-pool.h"

using nix::fmt;
using nix::logger;

struct cmd_copy_sigs_t : nix::StorePathsCommand {
  nix::strings_t substituter_uris;

  cmd_copy_sigs_t() {
    add_flag({
        .long_name = "substituter",
        .short_name = 's',
        .description = "Copy signatures from the specified store.",
        .labels = {"store-uri"},
        .handler = {[&](std::string s) { substituter_uris.push_back(s); }},
    });
  }

  std::string description() override { return "copy store path signatures from substituters"; }

  std::string doc() override {
    return
#include "store-copy-sigs.md"
        ;
  }

  void run(nix::ref<nix::store_t> store, nix::store_paths_t&& store_paths) override {
    if (substituter_uris.empty()) {
      throw nix::UsageError("you must specify at least one substituter using '-s'");
    }

    // FIXME: factor out commonality with MixVerify.
    std::vector<nix::ref<nix::store_t>> substituters;
    for (auto& s : substituter_uris) {
      substituters.push_back(nix::open_store(s));
    }

    nix::thread_pool_t pool{nix::file_transfer_settings.httpConnections};

    std::atomic<size_t> added{0};

    // logger->setExpected(doneLabel, storePaths.size());

    auto do_path = [&](const nix::Path& store_path_s) {
      // Activity act(*logger, lvlInfo, "getting signatures for '%s'", storePath);

      nix::check_interrupt();

      auto store_path = store->parseStorePath(store_path_s);

      auto info = store->queryPathInfo(store_path);

      nix::string_set_t new_sigs;

      for (auto& store2 : substituters) {
        try {
          auto info2 = store2->queryPathInfo(info->path);

          /* Don't import signatures that don't match this
             binary. */
          if (info->nar_hash != info2->nar_hash || info->nar_size != info2->nar_size ||
              info->references != info2->references) {
            continue;
          }

          for (auto& sig : info2->sigs) {
            if (!info->sigs.count(sig)) {
              new_sigs.insert(sig);
            }
          }
        } catch (nix::InvalidPath&) {
        }
      }

      if (!new_sigs.empty()) {
        store->addSignatures(store_path, new_sigs);
        added += new_sigs.size();
      }

      // logger->incProgress(doneLabel);
    };

    for (auto& store_path : store_paths) {
      pool.enqueue(std::bind(do_path, store->printStorePath(store_path)));
    }

    pool.process();

    printInfo("imported %d signatures", added);
  }
};

static auto r_cmd_copy_sigs = nix::registerCommand2<cmd_copy_sigs_t>({"store", "copy-sigs"});

struct cmd_sign_t : nix::StorePathsCommand {
  nix::Path secret_key_file;

  cmd_sign_t() {
    add_flag({
        .long_name = "key-file",
        .short_name = 'k',
        .description = "File containing the secret signing key.",
        .labels = {"file"},
        .handler = {&secret_key_file},
        .completer = complete_path,
        .required = true,
    });
  }

  std::string description() override { return "sign store paths with a local key"; }

  void run(nix::ref<nix::store_t> store, nix::store_paths_t&& store_paths) override {
    nix::secret_key_t secret_key(nix::read_file(secret_key_file));
    nix::local_signer_t signer(std::move(secret_key));

    size_t added{0};

    for (auto& store_path : store_paths) {
      auto info = store->queryPathInfo(store_path);

      auto info2(*info);
      info2.sigs.clear();
      info2.sign(*store, signer);
      assert(!info2.sigs.empty());

      if (!info->sigs.count(*info2.sigs.begin())) {
        store->addSignatures(store_path, info2.sigs);
        added++;
      }
    }

    printInfo("added %d signatures", added);
  }
};

static auto r_cmd_sign = nix::registerCommand2<cmd_sign_t>({"store", "sign"});

struct cmd_key_generate_secret_t : nix::command_t {
  std::string key_name;

  cmd_key_generate_secret_t() {
    add_flag({
        .long_name = "key-name",
        .description = "Identifier of the key (e.g. `cache.example.org-1`).",
        .labels = {"name"},
        .handler = {&key_name},
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
    nix::logger->stop();
    nix::write_full(nix::get_standard_output(), nix::secret_key_t::generate(key_name).to_string());
  }
};

struct cmd_key_convert_secret_to_public_t : nix::command_t {
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
    nix::secret_key_t secret_key(nix::drain_fd(STDIN_FILENO));
    nix::logger->stop();
    nix::write_full(nix::get_standard_output(), secret_key.to_public_key().to_string());
  }
};

struct cmd_key_t : nix::NixMultiCommand {
  cmd_key_t()
      : NixMultiCommand(
            "key",
            {
                {"generate-secret", []() { return nix::make_ref<cmd_key_generate_secret_t>(); }},
                {"convert-secret-to-public",
                 []() { return nix::make_ref<cmd_key_convert_secret_to_public_t>(); }},
            }) {}

  std::string description() override { return "generate and convert Nix signing keys"; }

  category_t category() override { return nix::catUtility; }
};

static auto r_cmd_key = nix::registerCommand<cmd_key_t>("key");
