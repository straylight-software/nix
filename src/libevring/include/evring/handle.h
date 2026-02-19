#pragma once

#include <cstdint>
#include <limits>
#include <optional>
#include <vector>

namespace evring {

/// generational handle - prevents ABA problems in async completion dispatch
struct handle {
  std::uint32_t index{std::numeric_limits<std::uint32_t>::max()};
  std::uint32_t generation{0};

  [[nodiscard]] constexpr auto operator==(handle const&) const noexcept -> bool = default;

  [[nodiscard]] constexpr auto valid() const noexcept -> bool {
    return index != std::numeric_limits<std::uint32_t>::max();
  }

  [[nodiscard]] static constexpr auto invalid() noexcept -> handle { return {}; }
};

/// sparse storage with generational indices
/// O(1) insert, remove, lookup with ABA protection
template <typename T>
class handle_table {
public:
  struct entry {
    T value;
    std::uint32_t generation;
    bool alive;
  };

private:
  std::vector<entry> entries_;
  std::vector<std::uint32_t> free_list_;

public:
  /// allocate a new handle and construct T in place
  template <typename... Args>
  auto insert(Args&&... args) -> handle {
    if (!free_list_.empty()) {
      std::uint32_t index = free_list_.back();
      free_list_.pop_back();
      entry& entry_ref = entries_[index];
      entry_ref.value = T(std::forward<Args>(args)...);
      entry_ref.alive = true;
      // generation already incremented on removal
      return handle{index, entry_ref.generation};
    }

    std::uint32_t index = static_cast<std::uint32_t>(entries_.size());
    entries_.push_back(entry{T(std::forward<Args>(args)...), 0, true});
    return handle{index, 0};
  }

  /// remove a handle, returns the value if valid
  auto remove(handle handle_to_remove) -> std::optional<T> {
    if (!valid(handle_to_remove)) {
      return std::nullopt;
    }

    entry& entry_ref = entries_[handle_to_remove.index];
    entry_ref.alive = false;
    entry_ref.generation++; // invalidate existing handles
    free_list_.push_back(handle_to_remove.index);
    return std::move(entry_ref.value);
  }

  /// check if handle is valid
  [[nodiscard]] auto valid(handle handle_to_check) const noexcept -> bool {
    return handle_to_check.index < entries_.size() && entries_[handle_to_check.index].alive &&
           entries_[handle_to_check.index].generation == handle_to_check.generation;
  }

  /// get value by handle
  [[nodiscard]] auto get(handle handle_to_get) noexcept -> T* {
    if (!valid(handle_to_get)) {
      return nullptr;
    }
    return &entries_[handle_to_get.index].value;
  }

  [[nodiscard]] auto get(handle handle_to_get) const noexcept -> T const* {
    if (!valid(handle_to_get)) {
      return nullptr;
    }
    return &entries_[handle_to_get.index].value;
  }

  /// number of active entries
  [[nodiscard]] auto size() const noexcept -> std::size_t {
    return entries_.size() - free_list_.size();
  }

  /// iterate over all active entries
  template <typename Func>
  void for_each(Func&& function) {
    for (std::uint32_t index = 0; index < entries_.size(); ++index) {
      if (entries_[index].alive) {
        function(handle{index, entries_[index].generation}, entries_[index].value);
      }
    }
  }
};

} // namespace evring
