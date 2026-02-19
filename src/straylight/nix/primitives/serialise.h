// straylight::nix::primitives::serialise - Binary serialization framework
//
// Modern C++23 binary serialization replacing nix/util/serialise.h.
// Provides:
//   - Source/Sink        - abstract base classes for reading/writing bytes
//   - StringSource/Sink  - in-memory implementations
//   - FdSource/FdSink    - file descriptor implementations
//   - BufferedSource/Sink - buffered wrappers
//   - Serialization helpers for integers and strings
//   - Varint encoding/decoding
//   - Little-endian wire format

#pragma once

#include <algorithm>
#include <array>
#include <bit>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

#ifdef _WIN32
#  include <io.h>
#else
#  include <unistd.h>
#endif

namespace straylight::nix::primitives {

// ─────────────────────────────────────────────────────────────────────────────
// Exceptions
// ─────────────────────────────────────────────────────────────────────────────

/// Exception thrown when reading past end of data.
class EndOfFile : public std::runtime_error {
public:
  explicit EndOfFile(const std::string& message) : std::runtime_error(message) {}
  explicit EndOfFile(const char* message) : std::runtime_error(message) {}
};

/// Exception thrown on serialization errors.
class SerialisationError : public std::runtime_error {
public:
  explicit SerialisationError(const std::string& message) : std::runtime_error(message) {}
  explicit SerialisationError(const char* message) : std::runtime_error(message) {}
};

// ─────────────────────────────────────────────────────────────────────────────
// Endianness utilities
// ─────────────────────────────────────────────────────────────────────────────

/// Write a little-endian integer to a byte buffer.
template <std::unsigned_integral T>
constexpr void write_little_endian(T value, std::span<std::byte, sizeof(T)> buffer) noexcept {
  if constexpr (std::endian::native == std::endian::little) {
    std::memcpy(buffer.data(), &value, sizeof(T));
  } else {
    for (std::size_t index = 0; index < sizeof(T); ++index) {
      buffer[index] = static_cast<std::byte>((value >> (index * 8)) & 0xFF);
    }
  }
}

/// Read a little-endian integer from a byte buffer.
template <std::unsigned_integral T>
constexpr T read_little_endian(std::span<const std::byte, sizeof(T)> buffer) noexcept {
  if constexpr (std::endian::native == std::endian::little) {
    T value;
    std::memcpy(&value, buffer.data(), sizeof(T));
    return value;
  } else {
    T value = 0;
    for (std::size_t index = 0; index < sizeof(T); ++index) {
      value |= static_cast<T>(static_cast<std::uint8_t>(buffer[index])) << (index * 8);
    }
    return value;
  }
}

/// Overload for char buffers (compatibility).
template <std::unsigned_integral T>
constexpr T read_little_endian(const unsigned char* buffer) noexcept {
  std::array<std::byte, sizeof(T)> bytes;
  for (std::size_t index = 0; index < sizeof(T); ++index) {
    bytes[index] = static_cast<std::byte>(buffer[index]);
  }
  return read_little_endian<T>(std::span<const std::byte, sizeof(T)>(bytes));
}

// ─────────────────────────────────────────────────────────────────────────────
// Varint encoding/decoding (unsigned LEB128)
// ─────────────────────────────────────────────────────────────────────────────

/// Maximum bytes needed to encode a 64-bit varint.
inline constexpr std::size_t max_varint_length = 10;

/// Encode an unsigned integer as a varint.
/// Returns the number of bytes written.
template <std::unsigned_integral T>
constexpr std::size_t encode_varint(T value, std::span<std::byte> buffer) noexcept {
  std::size_t index = 0;
  while (value >= 0x80 && index < buffer.size()) {
    buffer[index++] = static_cast<std::byte>((value & 0x7F) | 0x80);
    value >>= 7;
  }
  if (index < buffer.size()) {
    buffer[index++] = static_cast<std::byte>(value & 0x7F);
  }
  return index;
}

/// Result of decoding a varint.
template <typename T>
struct VarintDecodeResult {
  T value;
  std::size_t bytes_consumed;
};

/// Decode an unsigned varint from a buffer.
/// Returns nullopt if the buffer is incomplete or the value overflows T.
template <std::unsigned_integral T>
constexpr std::optional<VarintDecodeResult<T>>
decode_varint(std::span<const std::byte> buffer) noexcept {
  T value = 0;
  std::size_t shift = 0;
  std::size_t index = 0;

  while (index < buffer.size()) {
    std::uint8_t byte_value = static_cast<std::uint8_t>(buffer[index]);

    // Check for overflow
    if (shift >= sizeof(T) * 8) {
      return std::nullopt;
    }

    T segment = static_cast<T>(byte_value & 0x7F);

    // Check if adding this segment would overflow
    if (shift > 0 && segment > (std::numeric_limits<T>::max() >> shift)) {
      return std::nullopt;
    }

    value |= segment << shift;
    ++index;

    if ((byte_value & 0x80) == 0) {
      return VarintDecodeResult<T>{value, index};
    }

    shift += 7;
  }

  return std::nullopt; // Incomplete varint
}

// ─────────────────────────────────────────────────────────────────────────────
// Sink - abstract base for writing bytes
// ─────────────────────────────────────────────────────────────────────────────

/// Abstract destination for binary data.
class Sink {
public:
  virtual ~Sink() = default;

  /// Write data to the sink.
  virtual void write(std::span<const std::byte> data) = 0;

  /// Write a string_view as bytes.
  void write(std::string_view data) {
    write(std::span<const std::byte>(reinterpret_cast<const std::byte*>(data.data()), data.size()));
  }

  /// Check if the sink is in a good state.
  [[nodiscard]] virtual bool good() const noexcept { return true; }

  // Non-copyable but movable
  Sink() = default;
  Sink(const Sink&) = delete;
  Sink& operator=(const Sink&) = delete;
  Sink(Sink&&) = default;
  Sink& operator=(Sink&&) = default;
};

// ─────────────────────────────────────────────────────────────────────────────
// Source - abstract base for reading bytes
// ─────────────────────────────────────────────────────────────────────────────

/// Abstract source of binary data.
class Source {
public:
  virtual ~Source() = default;

  /// Read up to `buffer.size()` bytes into the buffer.
  /// Returns the number of bytes actually read.
  /// Blocks until at least one byte is available.
  /// Throws EndOfFile if no more data is available.
  [[nodiscard]] virtual std::size_t read(std::span<std::byte> buffer) = 0;

  /// Read exactly `buffer.size()` bytes into the buffer.
  /// Throws EndOfFile if not enough data is available.
  void read_exact(std::span<std::byte> buffer) {
    std::size_t total_read = 0;
    while (total_read < buffer.size()) {
      std::size_t bytes_read = read(buffer.subspan(total_read));
      if (bytes_read == 0) {
        throw EndOfFile("unexpected end of data");
      }
      total_read += bytes_read;
    }
  }

  /// Read exactly `length` bytes into a char buffer.
  void read_exact(char* data, std::size_t length) {
    read_exact(std::span<std::byte>(reinterpret_cast<std::byte*>(data), length));
  }

  /// Check if the source is in a good state.
  [[nodiscard]] virtual bool good() const noexcept { return true; }

  /// Skip `length` bytes.
  virtual void skip(std::size_t length) {
    std::array<std::byte, 4096> buffer;
    while (length > 0) {
      std::size_t to_read = std::min(length, buffer.size());
      std::size_t bytes_read = read(std::span<std::byte>(buffer.data(), to_read));
      if (bytes_read == 0) {
        throw EndOfFile("unexpected end of data during skip");
      }
      length -= bytes_read;
    }
  }

  /// Read all remaining data into a string.
  [[nodiscard]] std::string drain() {
    std::string result;
    std::array<std::byte, 4096> buffer;
    while (true) {
      try {
        std::size_t bytes_read = read(buffer);
        if (bytes_read == 0) {
          break;
        }
        result.append(reinterpret_cast<const char*>(buffer.data()), bytes_read);
      } catch (const EndOfFile&) {
        break;
      }
    }
    return result;
  }

  /// Drain all data into a sink.
  void drain_into(Sink& sink) {
    std::array<std::byte, 4096> buffer;
    while (true) {
      try {
        std::size_t bytes_read = read(buffer);
        if (bytes_read == 0) {
          break;
        }
        sink.write(std::span<const std::byte>(buffer.data(), bytes_read));
      } catch (const EndOfFile&) {
        break;
      }
    }
  }

  // Non-copyable but movable
  Source() = default;
  Source(const Source&) = delete;
  Source& operator=(const Source&) = delete;
  Source(Source&&) = default;
  Source& operator=(Source&&) = default;
};

// ─────────────────────────────────────────────────────────────────────────────
// StringSink - in-memory sink
// ─────────────────────────────────────────────────────────────────────────────

/// A sink that writes data to an in-memory string.
class StringSink : public Sink {
public:
  StringSink() = default;

  /// Construct with reserved capacity.
  explicit StringSink(std::size_t reserved_size) { data_.reserve(reserved_size); }

  /// Construct by taking ownership of an existing string.
  explicit StringSink(std::string&& data) : data_(std::move(data)) {}

  void write(std::span<const std::byte> data) override {
    data_.append(reinterpret_cast<const char*>(data.data()), data.size());
  }

  /// Get the accumulated data.
  [[nodiscard]] const std::string& data() const noexcept { return data_; }

  /// Get the accumulated data (non-const).
  [[nodiscard]] std::string& data() noexcept { return data_; }

  /// Extract the accumulated data (moves out).
  [[nodiscard]] std::string extract() noexcept { return std::move(data_); }

  /// Clear the accumulated data.
  void clear() noexcept { data_.clear(); }

  /// Get the size of accumulated data.
  [[nodiscard]] std::size_t size() const noexcept { return data_.size(); }

private:
  std::string data_;
};

// ─────────────────────────────────────────────────────────────────────────────
// StringSource - in-memory source
// ─────────────────────────────────────────────────────────────────────────────

/// A source that reads data from a string_view.
class StringSource : public Source {
public:
  /// Construct from a string_view.
  /// Note: The caller must ensure the underlying data outlives this source.
  explicit StringSource(std::string_view data) : data_(data), position_(0) {}

  /// Deleted constructor to prevent accidental dangling references.
  StringSource(std::string&&) = delete;

  [[nodiscard]] std::size_t read(std::span<std::byte> buffer) override {
    if (position_ >= data_.size()) {
      throw EndOfFile("end of string source");
    }
    std::size_t available = data_.size() - position_;
    std::size_t to_copy = std::min(buffer.size(), available);
    std::memcpy(buffer.data(), data_.data() + position_, to_copy);
    position_ += to_copy;
    return to_copy;
  }

  void skip(std::size_t length) override {
    if (position_ + length > data_.size()) {
      throw EndOfFile("skip past end of string source");
    }
    position_ += length;
  }

  /// Restart reading from the beginning.
  void restart() noexcept { position_ = 0; }

  /// Get remaining bytes.
  [[nodiscard]] std::size_t remaining() const noexcept { return data_.size() - position_; }

  /// Get current position.
  [[nodiscard]] std::size_t position() const noexcept { return position_; }

private:
  std::string_view data_;
  std::size_t position_;
};

// ─────────────────────────────────────────────────────────────────────────────
// BufferedSink - buffered wrapper for sinks
// ─────────────────────────────────────────────────────────────────────────────

/// A buffered sink that batches writes to an underlying sink.
/// Warning: Not thread-safe.
class BufferedSink : public Sink {
public:
  static constexpr std::size_t default_buffer_size = 32 * 1024;

  /// Construct without an underlying sink (must be set later or overridden).
  explicit BufferedSink(std::size_t buffer_size = default_buffer_size)
      : buffer_size_(buffer_size), buffer_position_(0) {}

  /// Construct wrapping another sink.
  BufferedSink(Sink& underlying, std::size_t buffer_size = default_buffer_size)
      : underlying_(&underlying), buffer_size_(buffer_size), buffer_position_(0) {}

  ~BufferedSink() override {
    try {
      flush();
    } catch (...) {
      // Ignore exceptions in destructor
    }
  }

  void write(std::span<const std::byte> data) override {
    // Allocate buffer lazily
    if (!buffer_) {
      buffer_ = std::make_unique<std::byte[]>(buffer_size_);
    }

    // If data fits in buffer, just copy it
    if (buffer_position_ + data.size() <= buffer_size_) {
      std::memcpy(buffer_.get() + buffer_position_, data.data(), data.size());
      buffer_position_ += data.size();
      return;
    }

    // Flush existing buffer
    flush();

    // If data is larger than buffer, write directly
    if (data.size() >= buffer_size_) {
      write_unbuffered(data);
      return;
    }

    // Otherwise buffer the data
    std::memcpy(buffer_.get(), data.data(), data.size());
    buffer_position_ = data.size();
  }

  /// Flush any buffered data to the underlying sink.
  void flush() {
    if (buffer_position_ > 0 && buffer_) {
      write_unbuffered(std::span<const std::byte>(buffer_.get(), buffer_position_));
      buffer_position_ = 0;
    }
  }

  [[nodiscard]] bool good() const noexcept override {
    return underlying_ ? underlying_->good() : true;
  }

protected:
  /// Override this in derived classes to write unbuffered data.
  virtual void write_unbuffered(std::span<const std::byte> data) {
    if (underlying_) {
      underlying_->write(data);
    }
  }

  Sink* underlying_ = nullptr;
  std::size_t buffer_size_;
  std::size_t buffer_position_;
  std::unique_ptr<std::byte[]> buffer_;
};

// ─────────────────────────────────────────────────────────────────────────────
// BufferedSource - buffered wrapper for sources
// ─────────────────────────────────────────────────────────────────────────────

/// A buffered source that batches reads from an underlying source.
/// Warning: Not thread-safe.
class BufferedSource : public Source {
public:
  static constexpr std::size_t default_buffer_size = 32 * 1024;

  /// Construct without an underlying source (must be set later or overridden).
  explicit BufferedSource(std::size_t buffer_size = default_buffer_size)
      : buffer_size_(buffer_size), buffer_position_(0), buffer_available_(0) {}

  /// Construct wrapping another source.
  BufferedSource(Source& underlying, std::size_t buffer_size = default_buffer_size)
      : underlying_(&underlying),
        buffer_size_(buffer_size),
        buffer_position_(0),
        buffer_available_(0) {}

  [[nodiscard]] std::size_t read(std::span<std::byte> buffer) override {
    // If we have buffered data, return from buffer first
    if (buffer_position_ < buffer_available_) {
      std::size_t available = buffer_available_ - buffer_position_;
      std::size_t to_copy = std::min(buffer.size(), available);
      std::memcpy(buffer.data(), buffer_.get() + buffer_position_, to_copy);
      buffer_position_ += to_copy;
      return to_copy;
    }

    // For large reads, bypass the buffer
    if (buffer.size() >= buffer_size_) {
      return read_unbuffered(buffer);
    }

    // Refill the buffer
    if (!buffer_) {
      buffer_ = std::make_unique<std::byte[]>(buffer_size_);
    }
    buffer_position_ = 0;
    buffer_available_ = read_unbuffered(std::span<std::byte>(buffer_.get(), buffer_size_));

    if (buffer_available_ == 0) {
      throw EndOfFile("end of buffered source");
    }

    std::size_t to_copy = std::min(buffer.size(), buffer_available_);
    std::memcpy(buffer.data(), buffer_.get(), to_copy);
    buffer_position_ = to_copy;
    return to_copy;
  }

  /// Check if the buffer has data available (without blocking).
  [[nodiscard]] bool has_data() const noexcept { return buffer_position_ < buffer_available_; }

  [[nodiscard]] bool good() const noexcept override {
    return underlying_ ? underlying_->good() : true;
  }

protected:
  /// Override this in derived classes to read unbuffered data.
  [[nodiscard]] virtual std::size_t read_unbuffered(std::span<std::byte> buffer) {
    if (underlying_) {
      return underlying_->read(buffer);
    }
    return 0;
  }

  Source* underlying_ = nullptr;
  std::size_t buffer_size_;
  std::size_t buffer_position_;
  std::size_t buffer_available_;
  std::unique_ptr<std::byte[]> buffer_;
};

// ─────────────────────────────────────────────────────────────────────────────
// FdSink - file descriptor sink
// ─────────────────────────────────────────────────────────────────────────────

/// A buffered sink that writes to a file descriptor.
class FdSink : public BufferedSink {
public:
  static constexpr int invalid_descriptor = -1;

  FdSink() : file_descriptor_(invalid_descriptor) {}

  explicit FdSink(int file_descriptor) : file_descriptor_(file_descriptor) {}

  FdSink(FdSink&& other) noexcept
      : BufferedSink(),
        file_descriptor_(std::exchange(other.file_descriptor_, invalid_descriptor)),
        bytes_written_(std::exchange(other.bytes_written_, 0)),
        good_(std::exchange(other.good_, true)) {
    // Manually transfer BufferedSink state
    buffer_size_ = other.buffer_size_;
    buffer_position_ = std::exchange(other.buffer_position_, 0);
    buffer_ = std::move(other.buffer_);
  }

  FdSink& operator=(FdSink&& other) noexcept {
    if (this != &other) {
      flush();
      // Manually transfer BufferedSink state
      buffer_size_ = other.buffer_size_;
      buffer_position_ = std::exchange(other.buffer_position_, 0);
      buffer_ = std::move(other.buffer_);
      file_descriptor_ = std::exchange(other.file_descriptor_, invalid_descriptor);
      bytes_written_ = std::exchange(other.bytes_written_, 0);
      good_ = std::exchange(other.good_, true);
    }
    return *this;
  }

  ~FdSink() override {
    try {
      flush();
    } catch (...) {
      // Ignore exceptions in destructor
    }
  }

  [[nodiscard]] bool good() const noexcept override { return good_; }

  /// Get the file descriptor.
  [[nodiscard]] int file_descriptor() const noexcept { return file_descriptor_; }

  /// Get total bytes written.
  [[nodiscard]] std::size_t bytes_written() const noexcept { return bytes_written_; }

protected:
  void write_unbuffered(std::span<const std::byte> data) override {
    if (file_descriptor_ == invalid_descriptor) {
      good_ = false;
      throw SerialisationError("FdSink: invalid file descriptor");
    }

    const std::byte* current = data.data();
    std::size_t remaining = data.size();

    while (remaining > 0) {
#ifdef _WIN32
      auto bytes_written =
          ::_write(file_descriptor_, current, static_cast<unsigned int>(remaining));
#else
      auto bytes_written = ::write(file_descriptor_, current, remaining);
#endif
      if (bytes_written < 0) {
        good_ = false;
        throw SerialisationError("FdSink: write error");
      }
      if (bytes_written == 0) {
        good_ = false;
        throw SerialisationError("FdSink: write returned 0");
      }
      current += bytes_written;
      remaining -= static_cast<std::size_t>(bytes_written);
      bytes_written_ += static_cast<std::size_t>(bytes_written);
    }
  }

private:
  int file_descriptor_;
  std::size_t bytes_written_ = 0;
  bool good_ = true;
};

// ─────────────────────────────────────────────────────────────────────────────
// FdSource - file descriptor source
// ─────────────────────────────────────────────────────────────────────────────

/// A buffered source that reads from a file descriptor.
class FdSource : public BufferedSource {
public:
  static constexpr int invalid_descriptor = -1;

  FdSource() : file_descriptor_(invalid_descriptor) {}

  explicit FdSource(int file_descriptor) : file_descriptor_(file_descriptor) {}

  FdSource(FdSource&& other) noexcept
      : BufferedSource(),
        file_descriptor_(std::exchange(other.file_descriptor_, invalid_descriptor)),
        bytes_read_(std::exchange(other.bytes_read_, 0)),
        good_(std::exchange(other.good_, true)) {
    // Manually transfer BufferedSource state
    buffer_size_ = other.buffer_size_;
    buffer_position_ = std::exchange(other.buffer_position_, 0);
    buffer_available_ = std::exchange(other.buffer_available_, 0);
    buffer_ = std::move(other.buffer_);
  }

  FdSource& operator=(FdSource&& other) noexcept {
    if (this != &other) {
      // Manually transfer BufferedSource state
      buffer_size_ = other.buffer_size_;
      buffer_position_ = std::exchange(other.buffer_position_, 0);
      buffer_available_ = std::exchange(other.buffer_available_, 0);
      buffer_ = std::move(other.buffer_);
      file_descriptor_ = std::exchange(other.file_descriptor_, invalid_descriptor);
      bytes_read_ = std::exchange(other.bytes_read_, 0);
      good_ = std::exchange(other.good_, true);
    }
    return *this;
  }

  [[nodiscard]] bool good() const noexcept override { return good_; }

  /// Get the file descriptor.
  [[nodiscard]] int file_descriptor() const noexcept { return file_descriptor_; }

  /// Get total bytes read.
  [[nodiscard]] std::size_t bytes_read() const noexcept { return bytes_read_; }

  /// Restart reading (seeks to beginning if seekable).
  void restart() {
    if (file_descriptor_ != invalid_descriptor) {
#ifdef _WIN32
      ::_lseek(file_descriptor_, 0, SEEK_SET);
#else
      ::lseek(file_descriptor_, 0, SEEK_SET);
#endif
      buffer_position_ = 0;
      buffer_available_ = 0;
      bytes_read_ = 0;
      good_ = true;
    }
  }

protected:
  [[nodiscard]] std::size_t read_unbuffered(std::span<std::byte> buffer) override {
    if (file_descriptor_ == invalid_descriptor) {
      good_ = false;
      throw EndOfFile("FdSource: invalid file descriptor");
    }

#ifdef _WIN32
    auto result =
        ::_read(file_descriptor_, buffer.data(), static_cast<unsigned int>(buffer.size()));
#else
    auto result = ::read(file_descriptor_, buffer.data(), buffer.size());
#endif

    if (result < 0) {
      good_ = false;
      throw SerialisationError("FdSource: read error");
    }

    if (result == 0) {
      throw EndOfFile("FdSource: end of file");
    }

    bytes_read_ += static_cast<std::size_t>(result);
    return static_cast<std::size_t>(result);
  }

private:
  int file_descriptor_;
  std::size_t bytes_read_ = 0;
  bool good_ = true;
};

// ─────────────────────────────────────────────────────────────────────────────
// NullSink - discards all data
// ─────────────────────────────────────────────────────────────────────────────

/// A sink that discards all data written to it.
class NullSink : public Sink {
public:
  void write(std::span<const std::byte> /*data*/) override {}
};

// ─────────────────────────────────────────────────────────────────────────────
// LengthSink - counts bytes written
// ─────────────────────────────────────────────────────────────────────────────

/// A sink that counts the number of bytes written without storing them.
class LengthSink : public Sink {
public:
  void write(std::span<const std::byte> data) override { length_ += data.size(); }

  /// Get the total length of data written.
  [[nodiscard]] std::uint64_t length() const noexcept { return length_; }

  /// Reset the counter.
  void reset() noexcept { length_ = 0; }

private:
  std::uint64_t length_ = 0;
};

// ─────────────────────────────────────────────────────────────────────────────
// TeeSink - writes to two sinks
// ─────────────────────────────────────────────────────────────────────────────

/// A sink that writes all incoming data to two other sinks.
class TeeSink : public Sink {
public:
  TeeSink(Sink& first, Sink& second) : first_(first), second_(second) {}

  void write(std::span<const std::byte> data) override {
    first_.write(data);
    second_.write(data);
  }

  [[nodiscard]] bool good() const noexcept override { return first_.good() && second_.good(); }

private:
  Sink& first_;
  Sink& second_;
};

// ─────────────────────────────────────────────────────────────────────────────
// TeeSource - reads and copies to a sink
// ─────────────────────────────────────────────────────────────────────────────

/// A source that copies all data read to a sink.
class TeeSource : public Source {
public:
  TeeSource(Source& source, Sink& sink) : source_(source), sink_(sink) {}

  [[nodiscard]] std::size_t read(std::span<std::byte> buffer) override {
    std::size_t bytes_read = source_.read(buffer);
    sink_.write(buffer.subspan(0, bytes_read));
    return bytes_read;
  }

  [[nodiscard]] bool good() const noexcept override { return source_.good(); }

private:
  Source& source_;
  Sink& sink_;
};

// ─────────────────────────────────────────────────────────────────────────────
// LengthSource - counts bytes read
// ─────────────────────────────────────────────────────────────────────────────

/// A source wrapper that counts the number of bytes read.
class LengthSource : public Source {
public:
  explicit LengthSource(Source& source) : source_(source) {}

  [[nodiscard]] std::size_t read(std::span<std::byte> buffer) override {
    std::size_t bytes_read = source_.read(buffer);
    total_ += bytes_read;
    return bytes_read;
  }

  [[nodiscard]] bool good() const noexcept override { return source_.good(); }

  /// Get the total bytes read.
  [[nodiscard]] std::uint64_t total() const noexcept { return total_; }

  /// Reset the counter.
  void reset() noexcept { total_ = 0; }

private:
  Source& source_;
  std::uint64_t total_ = 0;
};

// ─────────────────────────────────────────────────────────────────────────────
// SizedSource - limits reading to a specific size
// ─────────────────────────────────────────────────────────────────────────────

/// A source that reads at most a specified number of bytes from the underlying source.
class SizedSource : public Source {
public:
  SizedSource(Source& source, std::size_t size) : source_(source), remaining_(size) {}

  [[nodiscard]] std::size_t read(std::span<std::byte> buffer) override {
    if (remaining_ == 0) {
      throw EndOfFile("SizedSource: limit reached");
    }
    std::size_t to_read = std::min(buffer.size(), remaining_);
    std::size_t bytes_read = source_.read(buffer.subspan(0, to_read));
    remaining_ -= bytes_read;
    return bytes_read;
  }

  [[nodiscard]] bool good() const noexcept override { return source_.good(); }

  /// Get remaining bytes allowed to read.
  [[nodiscard]] std::size_t remaining() const noexcept { return remaining_; }

  /// Drain all remaining allowed data.
  std::size_t drain_all() {
    std::array<std::byte, 8192> buffer;
    std::size_t total = 0;
    while (remaining_ > 0) {
      std::size_t bytes_read = read(buffer);
      total += bytes_read;
    }
    return total;
  }

private:
  Source& source_;
  std::size_t remaining_;
};

// ─────────────────────────────────────────────────────────────────────────────
// Serialization helpers - integers
// ─────────────────────────────────────────────────────────────────────────────

/// Write a 64-bit unsigned integer in little-endian format.
inline void write_int(Sink& sink, std::uint64_t value) {
  std::array<std::byte, 8> buffer;
  write_little_endian(value, std::span<std::byte, 8>(buffer));
  sink.write(buffer);
}

/// Write an unsigned integer in little-endian format.
template <std::unsigned_integral T>
void write_int(Sink& sink, T value) {
  write_int(sink, static_cast<std::uint64_t>(value));
}

/// Write a signed integer (as unsigned).
template <std::signed_integral T>
void write_int(Sink& sink, T value) {
  write_int(sink, static_cast<std::uint64_t>(static_cast<std::make_unsigned_t<T>>(value)));
}

/// Read a numeric value from a source.
template <typename T>
  requires std::integral<T>
[[nodiscard]] T read_int(Source& source) {
  std::array<std::byte, 8> buffer;
  source.read_exact(std::span<std::byte>(buffer));
  std::uint64_t value = read_little_endian<std::uint64_t>(std::span<const std::byte, 8>(buffer));

  if constexpr (std::is_unsigned_v<T>) {
    if (value > std::numeric_limits<T>::max()) {
      throw SerialisationError("integer value too large for target type");
    }
    return static_cast<T>(value);
  } else {
    // For signed types, first check if it fits in the unsigned counterpart
    using unsigned_t = std::make_unsigned_t<T>;
    if (value > static_cast<std::uint64_t>(std::numeric_limits<unsigned_t>::max())) {
      throw SerialisationError("integer value too large for target type");
    }
    return static_cast<T>(static_cast<unsigned_t>(value));
  }
}

/// Read a 64-bit unsigned integer.
[[nodiscard]] inline std::uint64_t read_uint64(Source& source) {
  return read_int<std::uint64_t>(source);
}

/// Read a 32-bit unsigned integer.
[[nodiscard]] inline std::uint32_t read_uint32(Source& source) {
  return read_int<std::uint32_t>(source);
}

// ─────────────────────────────────────────────────────────────────────────────
// Serialization helpers - varint
// ─────────────────────────────────────────────────────────────────────────────

/// Write an unsigned integer as a varint.
template <std::unsigned_integral T>
void write_varint(Sink& sink, T value) {
  std::array<std::byte, max_varint_length> buffer;
  std::size_t length = encode_varint(value, buffer);
  sink.write(std::span<const std::byte>(buffer.data(), length));
}

/// Read an unsigned varint from a source.
template <std::unsigned_integral T>
[[nodiscard]] T read_varint(Source& source) {
  T value = 0;
  std::size_t shift = 0;

  while (true) {
    std::array<std::byte, 1> byte_buffer;
    source.read_exact(std::span<std::byte>(byte_buffer));
    std::uint8_t byte_value = static_cast<std::uint8_t>(byte_buffer[0]);

    if (shift >= sizeof(T) * 8) {
      throw SerialisationError("varint overflow");
    }

    T segment = static_cast<T>(byte_value & 0x7F);
    if (shift > 0 && segment > (std::numeric_limits<T>::max() >> shift)) {
      throw SerialisationError("varint overflow");
    }

    value |= segment << shift;

    if ((byte_value & 0x80) == 0) {
      return value;
    }

    shift += 7;
  }
}

// ─────────────────────────────────────────────────────────────────────────────
// Serialization helpers - strings
// ─────────────────────────────────────────────────────────────────────────────

/// Write a length-prefixed string.
inline void write_string(Sink& sink, std::string_view data) {
  write_int(sink, static_cast<std::uint64_t>(data.size()));
  sink.write(data);

  // Write padding to align to 8 bytes
  std::size_t padding = (8 - (data.size() % 8)) % 8;
  if (padding > 0) {
    static constexpr std::array<std::byte, 8> zeros{};
    sink.write(std::span<const std::byte>(zeros.data(), padding));
  }
}

/// Read padding bytes (used after reading a string).
inline void read_padding(Source& source, std::size_t data_length) {
  std::size_t padding = (8 - (data_length % 8)) % 8;
  if (padding > 0) {
    std::array<std::byte, 8> buffer;
    source.read_exact(std::span<std::byte>(buffer.data(), padding));
    // Optionally verify padding is zeros
  }
}

/// Read a length-prefixed string.
/// @param max_length Maximum allowed string length (for safety).
[[nodiscard]] inline std::string
read_string(Source& source, std::size_t max_length = std::numeric_limits<std::size_t>::max()) {
  std::uint64_t length = read_uint64(source);
  if (length > max_length) {
    throw SerialisationError("string length exceeds maximum");
  }

  std::string result(static_cast<std::size_t>(length), '\0');
  source.read_exact(result.data(), static_cast<std::size_t>(length));
  read_padding(source, static_cast<std::size_t>(length));
  return result;
}

/// Read a string into a pre-allocated buffer.
/// Returns the number of bytes read (not including padding).
[[nodiscard]] inline std::size_t read_string(Source& source, char* buffer, std::size_t max_length) {
  std::uint64_t length = read_uint64(source);
  if (length > max_length) {
    throw SerialisationError("string length exceeds buffer size");
  }

  source.read_exact(buffer, static_cast<std::size_t>(length));
  read_padding(source, static_cast<std::size_t>(length));
  return static_cast<std::size_t>(length);
}

// ─────────────────────────────────────────────────────────────────────────────
// Operator overloads for convenience
// ─────────────────────────────────────────────────────────────────────────────

/// Stream operator for writing integers to a sink.
inline Sink& operator<<(Sink& sink, std::uint64_t value) {
  write_int(sink, value);
  return sink;
}

/// Stream operator for writing strings to a sink.
inline Sink& operator<<(Sink& sink, std::string_view value) {
  write_string(sink, value);
  return sink;
}

/// Stream operator for reading integers from a source.
template <std::integral T>
Source& operator>>(Source& source, T& value) {
  value = read_int<T>(source);
  return source;
}

/// Stream operator for reading strings from a source.
inline Source& operator>>(Source& source, std::string& value) {
  value = read_string(source);
  return source;
}

} // namespace straylight::nix::primitives
