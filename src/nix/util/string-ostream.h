#pragma once

#include <ostream>
#include <streambuf>
#include <string>

namespace nix {

/// Minimal streambuf that writes to std::string.
/// Avoids locale overhead and virtual dispatch costs of std::ostringstream.
struct string_streambuf_t : std::streambuf {
  explicit string_streambuf_t(std::string& str) : str_(str) {}

protected:
  int_type overflow(int_type ch) override {
    if (ch != traits_type::eof()) {
      str_.push_back(static_cast<char>(ch));
    }
    return ch;
  }

  std::streamsize xsputn(const char* s, std::streamsize count) override {
    str_.append(s, static_cast<std::size_t>(count));
    return count;
  }

private:
  std::string& str_;
};

/// Minimal ostream that writes to std::string.
/// Use instead of std::ostringstream when ostream interface is required.
struct string_ostream_t : std::ostream {
  string_ostream_t() : std::ostream(&buf_), buf_(str_) {}

  [[nodiscard]] const std::string& str() const { return str_; }
  [[nodiscard]] std::string take() { return std::move(str_); }

  /// Clear the buffer for reuse
  void clear_buf() {
    str_.clear();
    std::ostream::clear(); // clear error flags
  }

private:
  std::string str_;
  string_streambuf_t buf_;
};

} // namespace nix
