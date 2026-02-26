#pragma once
/**
 * @file
 *
 * @brief pos_t and AbstractPos
 */

#include <cstdint>
#include <string>
#include <variant>

#include "nix/util/source-path.h"

namespace nix {

/**
 * A position and an origin for that position (like a source file).
 */
struct pos_t {
  uint32_t line = 0;
  uint32_t column = 0;

  struct Stdin {
    ref<std::string> source;

    bool operator==(const Stdin& rhs) const noexcept { return *source == *rhs.source; }

    std::strong_ordering operator<=>(const Stdin& rhs) const noexcept {
      return *source <=> *rhs.source;
    }
  };

  struct String {
    ref<std::string> source;

    bool operator==(const String& rhs) const noexcept { return *source == *rhs.source; }

    std::strong_ordering operator<=>(const String& rhs) const noexcept {
      return *source <=> *rhs.source;
    }
  };

  typedef std::variant<std::monostate, Stdin, String, source_path_t> origin_t;

  origin_t origin = std::monostate();

  pos_t() {}

  pos_t(uint32_t line, uint32_t column, origin_t origin)
      : line(line), column(column), origin(origin) {}

  explicit operator bool() const { return line > 0; }

  operator std::shared_ptr<const pos_t>() const;

  /**
   * Return the contents of the source file.
   */
  std::optional<std::string> get_source() const;

  void print(std::ostream& out, bool show_origin) const;

  // Returns the position as a string (with origin)
  [[nodiscard]] auto to_string() const -> std::string;

  std::optional<lines_of_code_t> get_code_lines() const;

  bool operator==(const pos_t& rhs) const = default;
  auto operator<=>(const pos_t& rhs) const = default;

  std::optional<std::string> get_snippet_up_to(const pos_t& end) const;

  /**
   * Get the source_path_t, if the source was loaded from a file.
   */
  std::optional<source_path_t> get_source_path() const;

  struct lines_iterator_t {
    using difference_type = size_t;
    using value_type = std::string_view;
    using reference = const std::string_view&;
    using pointer = const std::string_view*;
    using iterator_category = std::input_iterator_tag;

    lines_iterator_t() : pastEnd(true) {}

    explicit lines_iterator_t(std::string_view input) : input(input), pastEnd(input.empty()) {
      if (!pastEnd)
        bump(true);
    }

    lines_iterator_t& operator++() {
      bump(false);
      return *this;
    }

    lines_iterator_t operator++(int) {
      auto result = *this;
      ++*this;
      return result;
    }

    reference operator*() const { return curLine; }

    pointer operator->() const { return &curLine; }

    bool operator!=(const lines_iterator_t& other) const { return !(*this == other); }

    bool operator==(const lines_iterator_t& other) const {
      return (pastEnd && other.pastEnd) ||
             (std::forward_as_tuple(input.size(), input.data()) ==
              std::forward_as_tuple(other.input.size(), other.input.data()));
    }

  private:
    std::string_view input, curLine;
    bool pastEnd = false;

    void bump(bool at_first);
  };
};

std::ostream& operator<<(std::ostream& str, const pos_t& pos);

} // namespace nix
