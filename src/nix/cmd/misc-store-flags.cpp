#include "nix/cmd/misc-store-flags.h"

namespace nix::flag {

static void hash_format_completer(add_completions_t& completions, size_t index,
                                std::string_view prefix) {
  for (auto& format : hash_formats) {
    if (has_prefix(format, prefix)) {
      completions.add(format);
    }
  }
}

args_t::flag_t hash_format_with_default(std::string&& long_name, hash_format_t* hf) {
  assert(*hf == nix::hash_format_t::sri);
  return args_t::flag_t{
      .long_name = std::move(long_name),
      .description = "Hash format (`base16`, `nix32`, `base64`, `sri`). Default: `sri`.",
      .labels = {"hash-format"},
      .handler = {[hf](std::string s) { *hf = parse_hash_format(s); }},
      .completer = hash_format_completer,
  };
}

args_t::flag_t hash_format_opt(std::string&& long_name, std::optional<hash_format_t>* ohf) {
  return args_t::flag_t{
      .long_name = std::move(long_name),
      .description = "Hash format (`base16`, `nix32`, `base64`, `sri`).",
      .labels = {"hash-format"},
      .handler = {[ohf](std::string s) { *ohf = std::optional<hash_format_t>{parse_hash_format(s)}; }},
      .completer = hash_format_completer,
  };
}

static void hash_algo_completer(add_completions_t& completions, size_t index, std::string_view prefix) {
  for (auto& algo : hash_algorithms)
    if (has_prefix(algo, prefix))
      completions.add(algo);
}

args_t::flag_t hash_algo(std::string&& long_name, hash_algorithm_t* ha) {
  return args_t::flag_t{
      .long_name = std::move(long_name),
      .description = "Hash algorithm (`blake3`, `md5`, `sha1`, `sha256`, or `sha512`).",
      .labels = {"hash-algo"},
      .handler = {[ha](std::string s) { *ha = parse_hash_algo(s); }},
      .completer = hash_algo_completer,
  };
}

args_t::flag_t hash_algo_opt(std::string&& long_name, std::optional<hash_algorithm_t>* oha) {
  return args_t::flag_t{
      .long_name = std::move(long_name),
      .description = "Hash algorithm (`blake3`, `md5`, `sha1`, `sha256`, or `sha512`). Can be "
                     "omitted for SRI hashes.",
      .labels = {"hash-algo"},
      .handler = {[oha](std::string s) { *oha = std::optional<hash_algorithm_t>{parse_hash_algo(s)}; }},
      .completer = hash_algo_completer,
  };
}

args_t::flag_t file_ingestion_method(file_ingestion_method_t* method) {
  return args_t::flag_t{
      .long_name = "mode",
      // FIXME indentation carefully made for context, this is messed up.
      .description = R"(
    How to compute the hash of the input.
    One of:

    - `nar` (the default):
      Serialises the input as a
      [Nix Archive](@docroot@/store/file-system-object/content-address.md#serial-nix-archive)
      and passes that to the hash function.

    - `flat`:
      Assumes that the input is a single file and
      [directly passes](@docroot@/store/file-system-object/content-address.md#serial-flat)
      it to the hash function.
        )",
      .labels = {"file-ingestion-method"},
      .handler = {[method](std::string s) { *method = parse_file_ingestion_method(s); }},
  };
}

args_t::flag_t content_address_method(content_address_method_t* method) {
  return args_t::flag_t{
      .long_name = "mode",
      // FIXME indentation carefully made for context, this is messed up.
      .description = R"(
    How to compute the content-address of the store object.
    One of:

    - [`nar`](@docroot@/store/store-object/content-address.md#method-nix-archive)
      (the default):
      Serialises the input as a
      [Nix Archive](@docroot@/store/file-system-object/content-address.md#serial-nix-archive)
      and passes that to the hash function.

    - [`flat`](@docroot@/store/store-object/content-address.md#method-flat):
      Assumes that the input is a single file and
      [directly passes](@docroot@/store/file-system-object/content-address.md#serial-flat)
      it to the hash function.

    - [`text`](@docroot@/store/store-object/content-address.md#method-text):
      Like `flat`, but used for
      [derivations](@docroot@/glossary.md#gloss-store-derivation) serialized in store object and
      [`builtins.toFile`](@docroot@/language/builtins.html#builtins-toFile).
      For advanced use-cases only;
      for regular usage prefer `nar` and `flat`.
        )",
      .labels = {"content-address-method"},
      .handler = {[method](std::string s) { *method = content_address_method_t::parse(s); }},
  };
}

} // namespace nix::flag
