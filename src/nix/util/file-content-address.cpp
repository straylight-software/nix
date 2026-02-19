#include "nix/util/file-content-address.h"

#include "nix/util/archive.h"
#include "nix/util/git.h"
#include "nix/util/source-path.h"

namespace nix {

static std::optional<file_serialisation_method_t>
parseFileSerialisationMethodOpt(std::string_view input) {
  if (input == "flat") {
    return file_serialisation_method_t::Flat;
  } else if (input == "nar") {
    return file_serialisation_method_t::NixArchive;
  } else {
    return std::nullopt;
  }
}

file_serialisation_method_t parseFileSerialisationMethod(std::string_view input) {
  auto ret = parseFileSerialisationMethodOpt(input);
  if (ret)
    return *ret;
  else
    throw UsageError("Unknown file serialiation method '%s', expect `flat` or `nar`", input);
}

file_ingestion_method_t parseFileIngestionMethod(std::string_view input) {
  if (input == "git") {
    return file_ingestion_method_t::Git;
  } else {
    auto ret = parseFileSerialisationMethodOpt(input);
    if (ret)
      return static_cast<file_ingestion_method_t>(*ret);
    else
      throw UsageError("Unknown file ingestion method '%s', expect `flat`, `nar`, or `git`", input);
  }
}

std::string_view renderFileSerialisationMethod(file_serialisation_method_t method) {
  switch (method) {
    case file_serialisation_method_t::Flat:
      return "flat";
    case file_serialisation_method_t::NixArchive:
      return "nar";
    default:
      assert(false);
  }
}

std::string_view renderFileIngestionMethod(file_ingestion_method_t method) {
  switch (method) {
    case file_ingestion_method_t::Flat:
    case file_ingestion_method_t::NixArchive:
      return renderFileSerialisationMethod(static_cast<file_serialisation_method_t>(method));
    case file_ingestion_method_t::Git:
      return "git";
    default:
      unreachable();
  }
}

void dumpPath(const source_path_t& path, Sink& sink, file_serialisation_method_t method,
              path_filter_t& filter) {
  switch (method) {
    case file_serialisation_method_t::Flat:
      path.readFile(sink);
      break;
    case file_serialisation_method_t::NixArchive:
      path.dumpPath(sink, filter);
      break;
  }
}

void restorePath(const Path& path, Source& source, file_serialisation_method_t method,
                 bool startFsync) {
  switch (method) {
    case file_serialisation_method_t::Flat:
      writeFile(path, source, 0666, startFsync ? fs_sync_t::Yes : fs_sync_t::No);
      break;
    case file_serialisation_method_t::NixArchive:
      restorePath(path, source, startFsync);
      break;
  }
}

hash_result_t hashPath(const source_path_t& path, file_serialisation_method_t method, hash_algorithm_t ha,
                    path_filter_t& filter) {
  hash_sink_t sink{ha};
  dumpPath(path, sink, method, filter);
  return sink.finish();
}

std::pair<Hash, std::optional<uint64_t>>
hashPath(const source_path_t& path, file_ingestion_method_t method, hash_algorithm_t ht, path_filter_t& filter) {
  switch (method) {
    case file_ingestion_method_t::Flat:
    case file_ingestion_method_t::NixArchive: {
      auto res = hashPath(path, (file_serialisation_method_t)method, ht, filter);
      return {res.hash, res.numBytesDigested};
    }
    case file_ingestion_method_t::Git:
      return {git::dumpHash(ht, path, filter).hash, std::nullopt};
  }
  assert(false);
}

} // namespace nix
