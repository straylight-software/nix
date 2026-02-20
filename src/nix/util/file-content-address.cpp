#include "nix/util/file-content-address.h"

#include "nix/util/archive.h"
#include "nix/util/git.h"
#include "nix/util/source-path.h"

namespace nix {

static std::optional<file_serialisation_method_t>
parse_file_serialisation_method_opt(std::string_view input) {
  if (input == "flat") {
    return file_serialisation_method_t::flat;
  } else if (input == "nar") {
    return file_serialisation_method_t::nix_archive;
  } else {
    return std::nullopt;
  }
}

file_serialisation_method_t parse_file_serialisation_method(std::string_view input) {
  auto ret = parse_file_serialisation_method_opt(input);
  if (ret) {
    return *ret;
  } else {
    throw UsageError("Unknown file serialiation method '%s', expect `flat` or `nar`", input);
}
}

file_ingestion_method_t parse_file_ingestion_method(std::string_view input) {
  if (input == "git") {
    return file_ingestion_method_t::git;
  } else {
    auto ret = parse_file_serialisation_method_opt(input);
    if (ret) {
      return static_cast<file_ingestion_method_t>(*ret);
    } else {
      throw UsageError("Unknown file ingestion method '%s', expect `flat`, `nar`, or `git`", input);
}
  }
}

std::string_view render_file_serialisation_method(file_serialisation_method_t method) {
  switch (method) {
    case file_serialisation_method_t::flat:
      return "flat";
    case file_serialisation_method_t::nix_archive:
      return "nar";
    default:
      assert(false);
  }
}

std::string_view render_file_ingestion_method(file_ingestion_method_t method) {
  switch (method) {
    case file_ingestion_method_t::flat:
    case file_ingestion_method_t::nix_archive:
      return render_file_serialisation_method(static_cast<file_serialisation_method_t>(method));
    case file_ingestion_method_t::git:
      return "git";
    default:
      unreachable();
  }
}

void dump_path(const source_path_t& path, sink_t& sink, file_serialisation_method_t method,
              path_filter_t& filter) {
  switch (method) {
    case file_serialisation_method_t::flat:
      path.read_file(sink);
      break;
    case file_serialisation_method_t::nix_archive:
      path.dump_path(sink, filter);
      break;
  }
}

void restore_path(const Path& path, source_t& source, file_serialisation_method_t method,
                 bool start_fsync) {
  switch (method) {
    case file_serialisation_method_t::flat:
      write_file(path, source, 0666, start_fsync ? fs_sync_t::yes : fs_sync_t::no);
      break;
    case file_serialisation_method_t::nix_archive:
      restore_path(path, source, start_fsync);
      break;
  }
}

hash_result_t hash_path(const source_path_t& path, file_serialisation_method_t method, hash_algorithm_t ha,
                    path_filter_t& filter) {
  hash_sink_t sink{ha};
  dump_path(path, sink, method, filter);
  return sink.finish();
}

std::pair<Hash, std::optional<uint64_t>>
hash_path(const source_path_t& path, file_ingestion_method_t method, hash_algorithm_t ht, path_filter_t& filter) {
  switch (method) {
    case file_ingestion_method_t::flat:
    case file_ingestion_method_t::nix_archive: {
      auto res = hash_path(path, (file_serialisation_method_t)method, ht, filter);
      return {res.hash, res.num_bytes_digested};
    }
    case file_ingestion_method_t::git:
      return {git::dump_hash(ht, path, filter).hash, std::nullopt};
  }
  assert(false);
}

} // namespace nix
