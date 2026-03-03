#pragma once
///@file

#include <cstdint>
#include <vector>

#include "straylight/nix/data/lru_cache.h"

#include "nix/util/pos-idx.h"
#include "nix/util/position.h"
#include "nix/util/sync.h"

namespace nix {

class pos_table_t {
public:
  class origin_t {
    friend pos_table_t;

  private:
    uint32_t offset;

    origin_t(pos_t::origin_t origin, uint32_t offset, size_t size)
        : offset(offset), origin(origin), size(size) {}

  public:
    const pos_t::origin_t origin;
    const size_t size;

    uint32_t offset_of(pos_idx_t p) const { return p.id - 1 - offset; }
  };

private:
  /**
   * Vector of byte offsets (in the virtual input buffer) of initial line character's position.
   * Sorted by construction. Binary search over it allows for efficient translation of arbitrary
   * byte offsets in the virtual input buffer to its line + column position.
   */
  using lines_t = std::vector<uint32_t>;
  /**
   * cache_t from byte offset in the virtual buffer of Origins -> @ref lines_t in that origin.
   */
  using lines_cache_t = straylight::nix::data::LRUCache<uint32_t, lines_t>;

  mutable sync_t<lines_cache_t> lines_cache;

  // FIXME: this could be made lock-free (at least for access) if we
  // have a data structure where pointers to existing positions are
  // never invalidated.
  struct State {
    std::map<uint32_t, origin_t> origins;
  };

  shared_sync_t<State> state_;

  const origin_t* resolve(pos_idx_t p) const {
    if (p.id == 0) {
      return nullptr;
    }

    auto state(state_.read_lock());
    const auto idx = p.id - 1;
    /* We want the last key <= idx, so we'll take prev(first key >
       idx). This is guaranteed to never rewind origin.begin
       because the first key is always 0. */
    const auto past_origin = state->origins.upper_bound(idx);
    return &std::prev(past_origin)->second;
  }

public:
  pos_table_t(std::size_t linesCacheCapacity = 65536)
      : lines_cache(lines_cache_t(linesCacheCapacity)) {}

  origin_t add_origin(pos_t::origin_t origin, size_t size) {
    auto state(state_.lock());
    uint32_t offset = 0;
    if (auto it = state->origins.rbegin(); it != state->origins.rend()) {
      offset = it->first + it->second.size;
    }
    // +1 because all PosIdx are offset by 1 to begin with, and
    // another +1 to ensure that all origins can point to EOF, eg
    // on (invalid) empty inputs.
    if (2 + offset + size < offset) {
      return origin_t{origin, offset, 0};
    }
    return state->origins.emplace(offset, origin_t{origin, offset, size}).first->second;
  }

  pos_idx_t add(const origin_t& origin, size_t offset) {
    if (offset > origin.size) {
      return pos_idx_t();
    }
    return pos_idx_t(1 + origin.offset + offset);
  }

  /**
   * Convert a byte-offset pos_idx_t into a pos_t with line/column information.
   *
   * @param p Byte offset into the virtual concatenation of all parsed contents
   * @return Position
   *
   * @warning Very expensive to call, as this has to read the entire source
   * into memory each time. Call this only if absolutely necessary. Prefer
   * to keep pos_idx_t around instead of needlessly converting it into pos_t by
   * using this lookup method.
   */
  pos_t operator[](pos_idx_t p) const;

  pos_t::origin_t origin_of(pos_idx_t p) const {
    if (auto o = resolve(p)) {
      return o->origin;
    }
    return std::monostate{};
  }

  /**
   * Remove all origins from the table.
   */
  void clear() {
    auto lines = lines_cache.lock();
    lines->clear();
    state_.lock()->origins.clear();
  }
};

} // namespace nix
