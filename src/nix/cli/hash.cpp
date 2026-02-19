#include "nix/util/hash.h"

#include "man-pages.h"
#include "nix/cmd/command.h"
#include "nix/cmd/legacy.h"
#include "nix/cmd/misc-store-flags.h"
#include "nix/main/shared.h"
#include "nix/store/content-address.h"
#include "nix/store/references.h"
#include "nix/util/archive.h"
#include "nix/util/git.h"
#include "nix/util/posix-source-accessor.h"

using namespace nix;

/**
 * Base for `nix hash path`, `nix hash file` (deprecated), and `nix-hash` (legacy).
 *
 * Deprecation Issue: https://github.com/NixOS/nix/issues/8876
 */
struct cmd_hash_base_t : command_t {
  file_ingestion_method_t mode;
  hash_format_t hashFormat = hash_format_t::SRI;
  bool truncate = false;
  hash_algorithm_t hashAlgo = hash_algorithm_t::SHA256;
  std::vector<std::string> paths;
  std::optional<std::string> modulus;

  explicit cmd_hash_base_t(file_ingestion_method_t mode) : mode(mode) {
    expectArgs({.label = "paths", .handler = {&paths}, .completer = completePath});

    // FIXME The following flags should be deprecated, but we don't
    // yet have a mechanism for that.

    addFlag({
        .longName = "sri",
        .description = "Print the hash in SRI format.",
        .handler = {&hashFormat, hash_format_t::SRI},
    });

    addFlag({
        .longName = "base64",
        .description = "Print the hash in base-64 format.",
        .handler = {&hashFormat, hash_format_t::Base64},
    });

    addFlag({
        .longName = "base32",
        .description = "Print the hash in base-32 (Nix-specific) format.",
        .handler = {&hashFormat, hash_format_t::Nix32},
    });

    addFlag({
        .longName = "base16",
        .description = "Print the hash in base-16 format.",
        .handler = {&hashFormat, hash_format_t::Base16},
    });

    addFlag(flag::hashAlgo("type", &hashAlgo));
  }

  std::string description() override {
    switch (mode) {
      case file_ingestion_method_t::Flat:
        return "print cryptographic hash of a regular file";
      case file_ingestion_method_t::NixArchive:
        return "print cryptographic hash of the NAR serialisation of a path";
      case file_ingestion_method_t::Git:
        return "print cryptographic hash of the Git serialisation of a path";
      default:
        assert(false);
    };
  }

  void run() override {
    for (const auto& path : paths) {
      auto makeSink = [&]() -> std::unique_ptr<abstract_hash_sink_t> {
        if (modulus)
          return std::make_unique<HashModuloSink>(hashAlgo, *modulus);
        else
          return std::make_unique<hash_sink_t>(hashAlgo);
      };

      auto makeSourcePath = [&]() -> source_path_t {
        return posix_source_accessor_t::createAtRoot(makeParentCanonical(path));
      };

      Hash h{hash_algorithm_t::SHA256}; // throwaway def to appease C++
      switch (mode) {
        case file_ingestion_method_t::Flat: {
          // While usually we could use the some code as for NixArchive,
          // the Flat method needs to support FIFOs, such as those
          // produced by bash process substitution, e.g.:
          //     nix hash --mode flat <(echo hi)
          // Also symlinks semantics are unambiguous in the flat case,
          // so we don't need to go low-level, or reject symlink `path`s.
          auto hashSink = makeSink();
          readFile(path, *hashSink);
          h = hashSink->finish().hash;
          break;
        }
        case file_ingestion_method_t::NixArchive: {
          auto sourcePath = makeSourcePath();
          auto hashSink = makeSink();
          dumpPath(sourcePath, *hashSink, (file_serialisation_method_t)mode);
          h = hashSink->finish().hash;
          break;
        }
        case file_ingestion_method_t::Git: {
          auto sourcePath = makeSourcePath();
          std::function<git::dump_hook_t> hook;
          hook = [&](const source_path_t& path) -> git::TreeEntry {
            auto hashSink = makeSink();
            auto mode = dump(path, *hashSink, hook);
            auto hash = hashSink->finish().hash;
            return {
                .mode = mode,
                .hash = hash,
            };
          };
          h = hook(sourcePath).hash;
          break;
        }
      }

      if (truncate && h.hashSize > 20)
        h = compressHash(h, 20);
      logger->cout(h.to_string(hashFormat, hashFormat == hash_format_t::SRI));
    }
  }
};

/**
 * `nix hash path`
 */
struct cmd_hash_path_t : cmd_hash_base_t {
  cmd_hash_path_t() : cmd_hash_base_t(file_ingestion_method_t::NixArchive) {
    addFlag(flag::hashAlgo("algo", &hashAlgo));
    addFlag(flag::fileIngestionMethod(&mode));
    addFlag(flag::hashFormatWithDefault("format", &hashFormat));
#if 0
        addFlag({
            .longName = "modulo",
            .description = "Compute the hash modulo the specified string.",
            .labels = {"modulus"},
            .handler = {&modulus},
        });
#endif
  }
};

/**
 * For deprecated `nix hash file`
 *
 * Deprecation Issue: https://github.com/NixOS/nix/issues/8876
 */
struct cmd_hash_file_t : cmd_hash_base_t {
  cmd_hash_file_t() : cmd_hash_base_t(file_ingestion_method_t::Flat) {}
};

/**
 * For deprecated `nix hash to-*`
 */
struct cmd_to_base_t : command_t {
  hash_format_t hashFormat;
  std::optional<hash_algorithm_t> hashAlgo;
  std::vector<std::string> args;
  bool legacyCli;

  cmd_to_base_t(hash_format_t hashFormat, bool legacyCli = false)
      : hashFormat(hashFormat), legacyCli(legacyCli) {
    addFlag(flag::hashAlgoOpt("type", &hashAlgo));
    expectArgs("strings", &args);
  }

  std::string description() override {
    return fmt("convert a hash to %s representation (deprecated, use `nix hash convert` instead)",
               hashFormat == hash_format_t::Base16   ? "base-16"
               : hashFormat == hash_format_t::Nix32  ? "base-32"
               : hashFormat == hash_format_t::Base64 ? "base-64"
                                                  : "SRI");
  }

  void run() override {
    if (!legacyCli)
      warn("The old format conversion subcommands of `nix hash` were deprecated in favor of `nix "
           "hash convert`.");
    for (const auto& s : args)
      logger->cout(
          Hash::parseAny(s, hashAlgo).to_string(hashFormat, hashFormat == hash_format_t::SRI));
  }
};

/**
 * `nix hash convert`
 */
struct cmd_hash_convert_t : command_t {
  std::optional<hash_format_t> from;
  hash_format_t to;
  std::optional<hash_algorithm_t> algo;
  std::vector<std::string> hashStrings;

  cmd_hash_convert_t() : to(hash_format_t::SRI) {
    addFlag(flag::hashFormatOpt("from", &from));
    addFlag(flag::hashFormatWithDefault("to", &to));
    addFlag(flag::hashAlgoOpt(&algo));
    expectArgs({
        .label = "hashes",
        .handler = {&hashStrings},
    });
  }

  std::string description() override { return "convert between hash formats"; }

  std::string doc() override {
    return
#include "hash-convert.md"
        ;
  }

  category_t category() override { return catUtility; }

  void run() override {
    for (const auto& s : hashStrings) {
      auto [h, parsedFormat] = Hash::parseAnyReturningFormat(s, algo);
      if (from && *from != parsedFormat) {
        throw BadHash("input hash '%s' has format '%s', but '--from %s' was specified", s,
                      printHashFormat(parsedFormat), printHashFormat(*from));
      }
      logger->cout(h.to_string(to, to == hash_format_t::SRI));
    }
  }
};

struct cmd_hash_t : NixMultiCommand {
  cmd_hash_t()
      : NixMultiCommand("hash",
                        {
                            {"convert", []() { return make_ref<cmd_hash_convert_t>(); }},
                            {"path", []() { return make_ref<cmd_hash_path_t>(); }},
                            {"file", []() { return make_ref<cmd_hash_file_t>(); }},
                            {"to-base16", []() { return make_ref<cmd_to_base_t>(hash_format_t::Base16); }},
                            {"to-base32", []() { return make_ref<cmd_to_base_t>(hash_format_t::Nix32); }},
                            {"to-base64", []() { return make_ref<cmd_to_base_t>(hash_format_t::Base64); }},
                            {"to-sri", []() { return make_ref<cmd_to_base_t>(hash_format_t::SRI); }},
                        }) {}

  std::string description() override { return "compute and convert cryptographic hashes"; }

  category_t category() override { return catUtility; }
};

static auto rCmdHash = registerCommand<cmd_hash_t>("hash");

/* Legacy nix-hash command. */
static int compatNixHash(int argc, char** argv) {
  // Wait until `nix hash convert` is not hidden behind experimental flags anymore.
  // warn("`nix-hash` has been deprecated in favor of `nix hash convert`.");

  std::optional<hash_algorithm_t> hashAlgo;
  bool flat = false;
  hash_format_t hashFormat = hash_format_t::Base16;
  bool truncate = false;

  enum { opHash, opTo } op = opHash;

  std::vector<std::string> ss;

  parseCmdLine(argc, argv, [&](strings_t::iterator& arg, const strings_t::iterator& end) {
    if (*arg == "--help")
      showManPage("nix-hash");
    else if (*arg == "--version")
      printVersion("nix-hash");
    else if (*arg == "--flat")
      flat = true;
    else if (*arg == "--base16")
      hashFormat = hash_format_t::Base16;
    else if (*arg == "--base32")
      hashFormat = hash_format_t::Nix32;
    else if (*arg == "--base64")
      hashFormat = hash_format_t::Base64;
    else if (*arg == "--sri")
      hashFormat = hash_format_t::SRI;
    else if (*arg == "--truncate")
      truncate = true;
    else if (*arg == "--type") {
      std::string s = getArg(*arg, arg, end);
      hashAlgo = parseHashAlgo(s);
    } else if (*arg == "--to-base16") {
      op = opTo;
      hashFormat = hash_format_t::Base16;
    } else if (*arg == "--to-base32") {
      op = opTo;
      hashFormat = hash_format_t::Nix32;
    } else if (*arg == "--to-base64") {
      op = opTo;
      hashFormat = hash_format_t::Base64;
    } else if (*arg == "--to-sri") {
      op = opTo;
      hashFormat = hash_format_t::SRI;
    } else if (*arg != "" && arg->at(0) == '-')
      return false;
    else
      ss.push_back(*arg);
    return true;
  });

  if (op == opHash) {
    cmd_hash_base_t cmd(flat ? file_ingestion_method_t::Flat : file_ingestion_method_t::NixArchive);
    if (!hashAlgo.has_value())
      hashAlgo = hash_algorithm_t::MD5;
    cmd.hashAlgo = hashAlgo.value();
    cmd.hashFormat = hashFormat;
    cmd.truncate = truncate;
    cmd.paths = ss;
    cmd.run();
  }

  else {
    cmd_to_base_t cmd(hashFormat, true);
    cmd.args = ss;
    if (hashAlgo.has_value())
      cmd.hashAlgo = hashAlgo;
    cmd.run();
  }

  return 0;
}

static RegisterLegacyCommand r_nix_hash("nix-hash", compatNixHash);
