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

/**
 * Base for `nix hash path`, `nix hash file` (deprecated), and `nix-hash` (legacy).
 *
 * Deprecation Issue: https://github.com/NixOS/nix/issues/8876
 */
struct cmd_hash_base_t : nix::command_t {
  nix::file_ingestion_method_t mode;
  nix::hash_format_t hash_format = nix::hash_format_t::sri;
  bool truncate = false;
  nix::hash_algorithm_t hash_algo = nix::hash_algorithm_t::SHA256;
  std::vector<std::string> paths;
  std::optional<std::string> modulus;

  explicit cmd_hash_base_t(nix::file_ingestion_method_t mode) : mode(mode) {
    expect_args({.label = "paths", .handler = {&paths}, .completer = complete_path});

    // FIXME The following flags should be deprecated, but we don't
    // yet have a mechanism for that.

    add_flag({
        .long_name = "sri",
        .description = "Print the hash in SRI format.",
        .handler = {&hash_format, nix::hash_format_t::sri},
    });

    add_flag({
        .long_name = "base64",
        .description = "Print the hash in base-64 format.",
        .handler = {&hash_format, nix::hash_format_t::base64},
    });

    add_flag({
        .long_name = "base32",
        .description = "Print the hash in base-32 (Nix-specific) format.",
        .handler = {&hash_format, nix::hash_format_t::nix32},
    });

    add_flag({
        .long_name = "base16",
        .description = "Print the hash in base-16 format.",
        .handler = {&hash_format, nix::hash_format_t::base16},
    });

    add_flag(nix::flag::hash_algo("type", &hash_algo));
  }

  std::string description() override {
    switch (mode) {
      case nix::file_ingestion_method_t::flat:
        return "print cryptographic hash of a regular file";
      case nix::file_ingestion_method_t::nix_archive:
        return "print cryptographic hash of the NAR serialisation of a path";
      case nix::file_ingestion_method_t::git:
        return "print cryptographic hash of the Git serialisation of a path";
      default:
        assert(false);
    };
  }

  void run() override {
    for (const auto& path : paths) {
      auto makeSink = [&]() -> std::unique_ptr<nix::abstract_hash_sink_t> {
        if (modulus)
          return std::make_unique<nix::HashModuloSink>(hash_algo, *modulus);
        else
          return std::make_unique<nix::hash_sink_t>(hash_algo);
      };

      auto makeSourcePath = [&]() -> nix::source_path_t {
        return nix::posix_source_accessor_t::create_at_root(nix::make_parent_canonical(path));
      };

      nix::Hash h{nix::hash_algorithm_t::SHA256}; // throwaway def to appease C++
      switch (mode) {
        case nix::file_ingestion_method_t::flat: {
          // While usually we could use the some code as for NixArchive,
          // the Flat method needs to support FIFOs, such as those
          // produced by bash process substitution, e.g.:
          //     nix hash --mode flat <(echo hi)
          // Also symlinks semantics are unambiguous in the flat case,
          // so we don't need to go low-level, or reject symlink `path`s.
          auto hash_sink = makeSink();
          nix::read_file(path, *hash_sink);
          h = hash_sink->finish().hash;
          break;
        }
        case nix::file_ingestion_method_t::nix_archive: {
          auto source_path = makeSourcePath();
          auto hash_sink = makeSink();
          nix::dump_path(source_path, *hash_sink, (nix::file_serialisation_method_t)mode);
          h = hash_sink->finish().hash;
          break;
        }
        case nix::file_ingestion_method_t::git: {
          auto source_path = makeSourcePath();
          std::function<nix::git::dump_hook_t> hook;
          hook = [&](const nix::source_path_t& path) -> nix::git::tree_entry {
            auto hash_sink = makeSink();
            auto mode = nix::git::dump(path, *hash_sink, hook);
            auto hash = hash_sink->finish().hash;
            return {
                .mode = mode,
                .hash = hash,
            };
          };
          h = hook(source_path).hash;
          break;
        }
      }

      if (truncate && h.hash_size() > 20)
        h = nix::compress_hash(h, 20);
      nix::logger->cout(h.to_string(hash_format, hash_format == nix::hash_format_t::sri));
    }
  }
};

/**
 * `nix hash path`
 */
struct cmd_hash_path_t : cmd_hash_base_t {
  cmd_hash_path_t() : cmd_hash_base_t(nix::file_ingestion_method_t::nix_archive) {
    add_flag(nix::flag::hash_algo("algo", &hash_algo));
    add_flag(nix::flag::file_ingestion_method(&mode));
    add_flag(nix::flag::hash_format_with_default("format", &hash_format));
#if 0
        add_flag({
            .long_name = "modulo",
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
  cmd_hash_file_t() : cmd_hash_base_t(nix::file_ingestion_method_t::flat) {}
};

/**
 * For deprecated `nix hash to-*`
 */
struct cmd_to_base_t : nix::command_t {
  nix::hash_format_t hash_format;
  std::optional<nix::hash_algorithm_t> hash_algo;
  std::vector<std::string> args;
  bool legacy_cli;

  cmd_to_base_t(nix::hash_format_t hash_format, bool legacy_cli = false)
      : hash_format(hash_format), legacy_cli(legacy_cli) {
    add_flag(nix::flag::hash_algo_opt("type", &hash_algo));
    expect_args("strings", &args);
  }

  std::string description() override {
    return nix::fmt(
        "convert a hash to %s representation (deprecated, use `nix hash convert` instead)",
        hash_format == nix::hash_format_t::base16   ? "base-16"
        : hash_format == nix::hash_format_t::nix32  ? "base-32"
        : hash_format == nix::hash_format_t::base64 ? "base-64"
                                                    : "SRI");
  }

  void run() override {
    if (!legacy_cli)
      nix::warn("The old format conversion subcommands of `nix hash` were deprecated in favor of "
                "`nix "
                "hash convert`.");
    for (const auto& s : args)
      nix::logger->cout(nix::Hash::parse_any(s, hash_algo)
                            .to_string(hash_format, hash_format == nix::hash_format_t::sri));
  }
};

/**
 * `nix hash convert`
 */
struct cmd_hash_convert_t : nix::command_t {
  std::optional<nix::hash_format_t> from;
  nix::hash_format_t to;
  std::optional<nix::hash_algorithm_t> algo;
  std::vector<std::string> hash_strings;

  cmd_hash_convert_t() : to(nix::hash_format_t::sri) {
    add_flag(nix::flag::hash_format_opt("from", &from));
    add_flag(nix::flag::hash_format_with_default("to", &to));
    add_flag(nix::flag::hash_algo_opt(&algo));
    expect_args({
        .label = "hashes",
        .handler = {&hash_strings},
    });
  }

  std::string description() override { return "convert between hash formats"; }

  std::string doc() override {
    return
#include "hash-convert.md"
        ;
  }

  category_t category() override { return nix::catUtility; }

  void run() override {
    for (const auto& s : hash_strings) {
      auto [h, parsedFormat] = nix::Hash::parse_any_returning_format(s, algo);
      if (from && *from != parsedFormat) {
        throw nix::BadHash("input hash '%s' has format '%s', but '--from %s' was specified", s,
                           nix::print_hash_format(parsedFormat), nix::print_hash_format(*from));
      }
      nix::logger->cout(h.to_string(to, to == nix::hash_format_t::sri));
    }
  }
};

struct cmd_hash_t : nix::NixMultiCommand {
  cmd_hash_t()
      : NixMultiCommand(
            "hash",
            {
                {"convert", []() { return nix::make_ref<cmd_hash_convert_t>(); }},
                {"path", []() { return nix::make_ref<cmd_hash_path_t>(); }},
                {"file", []() { return nix::make_ref<cmd_hash_file_t>(); }},
                {"to-base16",
                 []() { return nix::make_ref<cmd_to_base_t>(nix::hash_format_t::base16); }},
                {"to-base32",
                 []() { return nix::make_ref<cmd_to_base_t>(nix::hash_format_t::nix32); }},
                {"to-base64",
                 []() { return nix::make_ref<cmd_to_base_t>(nix::hash_format_t::base64); }},
                {"to-sri", []() { return nix::make_ref<cmd_to_base_t>(nix::hash_format_t::sri); }},
            }) {}

  std::string description() override { return "compute and convert cryptographic hashes"; }

  category_t category() override { return nix::catUtility; }
};

static auto r_cmd_hash = nix::registerCommand<cmd_hash_t>("hash");

/* Legacy nix-hash command. */
static int compat_nix_hash(int argc, char** argv) {
  // Wait until `nix hash convert` is not hidden behind experimental flags anymore.
  // warn("`nix-hash` has been deprecated in favor of `nix hash convert`.");

  std::optional<nix::hash_algorithm_t> hash_algo;
  bool flat = false;
  nix::hash_format_t hash_format = nix::hash_format_t::base16;
  bool truncate = false;

  enum { opHash, op_to } op = opHash;

  std::vector<std::string> ss;

  nix::parse_cmd_line(argc, argv,
                      [&](nix::strings_t::iterator& arg, const nix::strings_t::iterator& end) {
                        if (*arg == "--help")
                          nix::show_man_page("nix-hash");
                        else if (*arg == "--version")
                          nix::print_version("nix-hash");
                        else if (*arg == "--flat")
                          flat = true;
                        else if (*arg == "--base16")
                          hash_format = nix::hash_format_t::base16;
                        else if (*arg == "--base32")
                          hash_format = nix::hash_format_t::nix32;
                        else if (*arg == "--base64")
                          hash_format = nix::hash_format_t::base64;
                        else if (*arg == "--sri")
                          hash_format = nix::hash_format_t::sri;
                        else if (*arg == "--truncate")
                          truncate = true;
                        else if (*arg == "--type") {
                          std::string s = nix::get_arg(*arg, arg, end);
                          hash_algo = nix::parse_hash_algo(s);
                        } else if (*arg == "--to-base16") {
                          op = op_to;
                          hash_format = nix::hash_format_t::base16;
                        } else if (*arg == "--to-base32") {
                          op = op_to;
                          hash_format = nix::hash_format_t::nix32;
                        } else if (*arg == "--to-base64") {
                          op = op_to;
                          hash_format = nix::hash_format_t::base64;
                        } else if (*arg == "--to-sri") {
                          op = op_to;
                          hash_format = nix::hash_format_t::sri;
                        } else if (*arg != "" && arg->at(0) == '-')
                          return false;
                        else
                          ss.push_back(*arg);
                        return true;
                      });

  if (op == opHash) {
    cmd_hash_base_t cmd(flat ? nix::file_ingestion_method_t::flat
                             : nix::file_ingestion_method_t::nix_archive);
    if (!hash_algo.has_value())
      hash_algo = nix::hash_algorithm_t::MD5;
    cmd.hash_algo = hash_algo.value();
    cmd.hash_format = hash_format;
    cmd.truncate = truncate;
    cmd.paths = ss;
    cmd.run();
  }

  else {
    cmd_to_base_t cmd(hash_format, true);
    cmd.args = ss;
    if (hash_algo.has_value())
      cmd.hash_algo = hash_algo;
    cmd.run();
  }

  return 0;
}

static nix::RegisterLegacyCommand r_nix_hash("nix-hash", compat_nix_hash);
