#pragma once
///@file

#include "nix/util/hash.h"

namespace nix {

class RefScanSink : public Sink {
  string_set_t hashes;
  string_set_t seen;

  std::string tail;

public:
  RefScanSink(string_set_t&& hashes) : hashes(hashes) {}

  string_set_t& getResult() { return seen; }

  void operator()(std::string_view data) override;
};

struct RewritingSink : Sink {
  const string_map_t rewrites;
  std::string::size_type maxRewriteSize;
  std::string prev;
  Sink& next_sink;
  uint64_t pos = 0;

  std::vector<uint64_t> matches;

  RewritingSink(const std::string& from, const std::string& to, Sink& next_sink);
  RewritingSink(const string_map_t& rewrites, Sink& next_sink);

  void operator()(std::string_view data) override;

  void flush();
};

struct HashModuloSink : abstract_hash_sink_t {
  hash_sink_t hash_sink;
  RewritingSink rewritingSink;

  HashModuloSink(hash_algorithm_t ha, const std::string& modulus);

  void operator()(std::string_view data) override;

  hash_result_t finish() override;
};

} // namespace nix
