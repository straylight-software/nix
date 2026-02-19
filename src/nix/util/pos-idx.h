#pragma once
///@file

#include <cinttypes>
#include <functional>

namespace nix {

class pos_idx_t {
  friend struct lazy_pos_accessors_t;
  friend class pos_table_t;
  friend class std::hash<pos_idx_t>;

private:
  uint32_t id;

public:
  explicit pos_idx_t(uint32_t id) : id(id) {}

  pos_idx_t() : id(0) {}

  explicit operator bool() const { return id > 0; }

  auto operator<=>(const pos_idx_t other) const { return id <=> other.id; }

  bool operator==(const pos_idx_t other) const { return id == other.id; }

  size_t hash() const noexcept { return std::hash<uint32_t>{}(id); }

  uint32_t get() const { return id; }
};

inline pos_idx_t no_pos = {};

} // namespace nix

namespace std {

template <>
struct hash<nix::pos_idx_t> {
  std::size_t operator()(nix::pos_idx_t pos) const noexcept { return pos.hash(); }
};

} // namespace std
