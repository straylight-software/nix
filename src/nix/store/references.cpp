#include "nix/store/references.h"

#include <algorithm>
#include <cstdlib>
#include <map>
#include <mutex>

#include "nix/store/path.h"
#include "nix/util/base-nix-32.h"
#include "nix/util/hash.h"

namespace nix {

static constexpr auto ref_length = store_path_t::HashLen;

static void search(std::string_view s, string_set_t& hashes, string_set_t& seen) {
  for (size_t i = 0; i + ref_length <= s.size();) {
    int j;
    bool match = true;
    for (j = ref_length - 1; j >= 0; --j) {
      if (!base_nix32_t::lookup_reverse(s[i + j])) {
        i += j + 1;
        match = false;
        break;
      }
    }
    if (!match) {
      continue;
    }
    std::string ref(s.substr(i, ref_length));
    if (hashes.erase(ref)) {
      debug("found reference to '%1%' at offset '%2%'", ref, i);
      seen.insert(ref);
    }
    ++i;
  }
}

void RefScanSink::operator()(std::string_view data) {
  /* It's possible that a reference spans the previous and current
     fragment, so search in the concatenation of the tail of the
     previous fragment and the start of the current fragment. */
  auto s = tail;
  auto tailLen = std::min(data.size(), ref_length);
  s.append(data.data(), tailLen);
  search(s, hashes, seen);

  search(data, hashes, seen);

  auto rest = ref_length - tailLen;
  if (rest < tail.size()) {
    tail = tail.substr(tail.size() - rest);
  }
  tail.append(data.data() + data.size() - tailLen, tailLen);
}

RewritingSink::RewritingSink(const std::string& from, const std::string& to, sink_t& next_sink)
    : RewritingSink({{from, to}}, next_sink) {}

RewritingSink::RewritingSink(const string_map_t& rewrites, sink_t& next_sink)
    : rewrites(rewrites), next_sink(next_sink) {
  std::string::size_type maxRewriteSize = 0;
  for (auto& [from, to] : rewrites) {
    assert(from.size() == to.size());
    maxRewriteSize = std::max(maxRewriteSize, from.size());
  }
  this->maxRewriteSize = maxRewriteSize;
}

void RewritingSink::operator()(std::string_view data) {
  std::string s(prev);
  s.append(data);

  s = rewrite_strings(s, rewrites);

  prev = s.size() < maxRewriteSize ? s
         : maxRewriteSize == 0     ? ""
                               : std::string(s, s.size() - maxRewriteSize + 1, maxRewriteSize - 1);

  auto consumed = s.size() - prev.size();

  pos += consumed;

  if (consumed) {
    next_sink(s.substr(0, consumed));
  }
}

void RewritingSink::flush() {
  if (prev.empty()) {
    return;
  }
  pos += prev.size();
  next_sink(prev);
  prev.clear();
}

HashModuloSink::HashModuloSink(hash_algorithm_t ha, const std::string& modulus)
    : hash_sink(ha), rewritingSink(modulus, std::string(modulus.size(), 0), hash_sink) {}

void HashModuloSink::operator()(std::string_view data) {
  rewritingSink(data);
}

hash_result_t HashModuloSink::finish() {
  rewritingSink.flush();

  /* Hash the positions of the self-references. This ensures that a
     NAR with self-references and a NAR with some of the
     self-references already zeroed out do not produce a hash
     collision. FIXME: proof. */
  for (auto& pos : rewritingSink.matches) {
    hash_sink(fmt("|%d", pos));
  }

  auto h = hash_sink.finish();
  return {.hash = h.hash, .num_bytes_digested = rewritingSink.pos};
}

} // namespace nix
