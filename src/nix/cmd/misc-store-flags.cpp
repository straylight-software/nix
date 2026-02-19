#include "nix/cmd/misc-store-flags.h"

namespace nix::flag {

static void hashFormatCompleter(add_completions_t& completions, size_t index,
                                std::string_view prefix) {
  for (auto& format : hashFormats) {
    if (hasPrefix(format, prefix)) {
      completions.add(format);
    }
  }
}

Args::flag_t hashFormatWithDefault(std::string&& longName, hash_format_t* hf) {
  assert(*hf == nix::hash_format_t::SRI);
  return Args::flag_t{
      .longName = std::move(longName),
      .description = "Hash format (`base16`, `nix32`, `base64`, `sri`). Default: `sri`.",
      .labels = {"hash-format"},
      .handler = {[hf](std::string s) { *hf = parseHashFormat(s); }},
      .completer = hashFormatCompleter,
  };
}

Args::flag_t hashFormatOpt(std::string&& longName, std::optional<hash_format_t>* ohf) {
  return Args::flag_t{
      .longName = std::move(longName),
      .description = "Hash format (`base16`, `nix32`, `base64`, `sri`).",
      .labels = {"hash-format"},
      .handler = {[ohf](std::string s) { *ohf = std::optional<hash_format_t>{parseHashFormat(s)}; }},
      .completer = hashFormatCompleter,
  };
}

static void hashAlgoCompleter(add_completions_t& completions, size_t index, std::string_view prefix) {
  for (auto& algo : hashAlgorithms)
    if (hasPrefix(algo, prefix))
      completions.add(algo);
}

Args::flag_t hashAlgo(std::string&& longName, hash_algorithm_t* ha) {
  return Args::flag_t{
      .longName = std::move(longName),
      .description = "Hash algorithm (`blake3`, `md5`, `sha1`, `sha256`, or `sha512`).",
      .labels = {"hash-algo"},
      .handler = {[ha](std::string s) { *ha = parseHashAlgo(s); }},
      .completer = hashAlgoCompleter,
  };
}

Args::flag_t hashAlgoOpt(std::string&& longName, std::optional<hash_algorithm_t>* oha) {
  return Args::flag_t{
      .longName = std::move(longName),
      .description = "Hash algorithm (`blake3`, `md5`, `sha1`, `sha256`, or `sha512`). Can be "
                     "omitted for SRI hashes.",
      .labels = {"hash-algo"},
      .handler = {[oha](std::string s) { *oha = std::optional<hash_algorithm_t>{parseHashAlgo(s)}; }},
      .completer = hashAlgoCompleter,
  };
}

Args::flag_t fileIngestionMethod(file_ingestion_method_t* method) {
  return Args::flag_t{
      .longName = "mode",
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
      .handler = {[method](std::string s) { *method = parseFileIngestionMethod(s); }},
  };
}

Args::flag_t contentAddressMethod(ContentAddressMethod* method) {
  return Args::flag_t{
      .longName = "mode",
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
      .handler = {[method](std::string s) { *method = ContentAddressMethod::parse(s); }},
  };
}

} // namespace nix::flag
