#pragma once

#include <cstdint>
#include <limits>
#include <optional>
#include <vector>

namespace evring {

/// generational handle - prevents ABA problems in async completion dispatch
struct handle {
  std::uint32_t index_{std::numeric_limits<std::uint32_t>::max()};
  std::uint32_t generation_{0};

  [[nodiscard]] constexpr auto operator==(handle const&) const noexcept -> bool = default;

  [[nodiscard]] constexpr auto valid() const noexcept -> bool {
    return index_ != std::numeric_limits<std::uint32_t>::max();
  }

  [[nodiscard]] static constexpr auto invalid() noexcept -> handle { return {}; }
};

/// sparse storage with generational indices
/// O(1) insert, remove, lookup with ABA protection
template <typename T>
struct handle_table {
  struct entry {
    T value_;
    std::uint32_t generation_;
    bool alive_;
  };

  std::vector<entry> entries_;
  std::vector<std::uint32_t> free_list_;

  /// allocate a new handle and construct T in place
  template <typename... Args>
  auto insert(Args&&... args) -> handle {
    if (!free_list_.empty()) {
      std::uint32_t idx = free_list_.back();
      free_list_.pop_back();
      entry& entry_ref = entries_[idx];
      entry_ref.value_ = T(std::forward<Args>(args)...);
      entry_ref.alive_ = true;
      // generation already incremented on removal
      return handle{idx, entry_ref.generation_};
    }

    auto idx = static_cast<std::uint32_t>(entries_.size());
    entries_.push_back(entry{T(std::forward<Args>(args)...), 0, true});
    return handle{idx, 0};
  }

  /// remove a handle, returns the value if valid
  auto remove(handle handle_to_remove) -> std::optional<T> {
    if (!valid(handle_to_remove)) {
      return std::nullopt;
    }

    entry& entry_ref = entries_[handle_to_remove.index_];
    entry_ref.alive_ = false;
    entry_ref.generation_++; // invalidate existing handles
    free_list_.push_back(handle_to_remove.index_);
    return std::move(entry_ref.value_);
  }

  /// check if handle is valid
  [[nodiscard]] auto valid(handle handle_to_check) const noexcept -> bool {
    return handle_to_check.index_ < entries_.size() && entries_[handle_to_check.index_].alive_ &&
           entries_[handle_to_check.index_].generation_ == handle_to_check.generation_;
  }

  /// get value by handle
  [[nodiscard]] auto get(handle handle_to_get) noexcept -> T* {
    if (!valid(handle_to_get)) {
      return nullptr;
    }
    return &entries_[handle_to_get.index_].value_;
  }

  [[nodiscard]] auto get(handle handle_to_get) const noexcept -> T const* {
    if (!valid(handle_to_get)) {
      return nullptr;
    }
    return &entries_[handle_to_get.index_].value_;
  }

  /// number of active entries
  [[nodiscard]] auto size() const noexcept -> std::size_t {
    return entries_.size() - free_list_.size();
  }

  /// iterate over all active entries
  template <typename Func>
  void for_each(Func&& function) {
    for (std::uint32_t idx = 0; idx < entries_.size(); ++idx) {
      if (entries_[idx].alive_) {
        function(handle{idx, entries_[idx].generation_}, entries_[idx].value_);
      }
    }
  }
};

} // namespace evring
