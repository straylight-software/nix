#pragma once
///@file

#include <memory>
#include <type_traits>

#include "nix/util/file-descriptor.h"
#include "nix/util/types.h"
#include "nix/util/util.h"

namespace boost::context {
struct stack_context;
}

namespace nix {

/**
 * Abstract destination of binary data.
 */
struct Sink {
  virtual ~Sink() = default;

  virtual auto operator()(std::string_view data) -> void = 0;

  [[nodiscard]] virtual auto good() -> bool { return true; }
};

/**
 * Just throws away data.
 */
struct null_sink_t : Sink {
  auto operator()([[maybe_unused]] std::string_view data) -> void override {}
};

struct finish_sink_t : virtual Sink {
  virtual auto finish() -> void = 0;
};

/**
 * A buffered abstract sink. Warning: a buffered_sink_t should not be
 * used from multiple threads concurrently.
 */
class buffered_sink_t : public virtual Sink {
public:
  buffered_sink_t(size_t buf_size = 32 * 1024)
      : buf_size_(buf_size), buf_pos_(0), buffer_(nullptr) {}

  auto operator()(std::string_view data) -> void override;

  auto flush() -> void;

  [[nodiscard]] auto buf_size() const -> size_t { return buf_size_; }
  [[nodiscard]] auto buf_pos() const -> size_t { return buf_pos_; }

protected:
  virtual auto write_unbuffered(std::string_view data) -> void = 0;

  size_t buf_size_;
  size_t buf_pos_;
  std::unique_ptr<char[]> buffer_;
};

/**
 * Abstract source of binary data.
 */
struct Source {
  virtual ~Source() = default;

  /**
   * Store exactly 'len' bytes in the buffer pointed to by 'data'.
   * It blocks until all the requested data is available, or throws
   * an error if it is not going to be available.
   */
  auto operator()(char* data, size_t len) -> void;
  auto operator()(std::string_view data) -> void;

  /**
   * Store up to 'len' in the buffer pointed to by 'data', and
   * return the number of bytes stored.  It blocks until at least
   * one byte is available.
   */
  virtual auto read(char* data, size_t len) -> size_t = 0;

  [[nodiscard]] virtual auto good() -> bool { return true; }

  auto drain_into(Sink& sink) -> void;

  [[nodiscard]] auto drain() -> std::string;

  virtual auto skip(size_t len) -> void;
};

/**
 * A buffered abstract source. Warning: a buffered_source_t should not be
 * used from multiple threads concurrently.
 */
class buffered_source_t : public virtual Source {
public:
  buffered_source_t(size_t buf_size = 32 * 1024)
      : buf_size_(buf_size), buf_pos_in_(0), buf_pos_out_(0), buffer_(nullptr) {}

  auto read(char* data, size_t len) -> size_t override;

  /**
   * Return true if the buffer is not empty.
   */
  [[nodiscard]] auto has_data() -> bool;

  [[nodiscard]] auto buf_size() const -> size_t { return buf_size_; }
  [[nodiscard]] auto buf_pos_in() const -> size_t { return buf_pos_in_; }
  [[nodiscard]] auto buf_pos_out() const -> size_t { return buf_pos_out_; }

protected:
  /**
   * Underlying read call, to be overridden.
   */
  virtual auto read_unbuffered(char* data, size_t len) -> size_t = 0;

  size_t buf_size_;
  size_t buf_pos_in_;
  size_t buf_pos_out_;
  std::unique_ptr<char[]> buffer_;
};

/**
 * Source type that can be restarted.
 */
struct restartable_source_t : virtual Source {
  virtual auto restart() -> void = 0;
};

/**
 * A sink that writes data to a file descriptor.
 */
class fd_sink_t : public buffered_sink_t {
public:
  fd_sink_t() : fd_(INVALID_DESCRIPTOR) {}

  explicit fd_sink_t(descriptor_t fd) : fd_(fd) {}

  fd_sink_t(fd_sink_t&&) = default;

  auto operator=(fd_sink_t&& s) -> fd_sink_t& {
    flush();
    fd_ = s.fd_;
    s.fd_ = INVALID_DESCRIPTOR;
    written_ = s.written_;
    return *this;
  }

  ~fd_sink_t() override;

  auto write_unbuffered(std::string_view data) -> void override;

  [[nodiscard]] auto good() -> bool override;

  [[nodiscard]] auto fd() const -> descriptor_t { return fd_; }
  auto set_fd(descriptor_t fd) -> void { fd_ = fd; }
  [[nodiscard]] auto written() const -> size_t { return written_; }

private:
  descriptor_t fd_;
  size_t written_ = 0;
  bool good_ = true;
};

/**
 * A source that reads data from a file descriptor.
 */
class fd_source_t : public buffered_source_t, public restartable_source_t {
public:
  fd_source_t() : fd_(INVALID_DESCRIPTOR) {}

  explicit fd_source_t(descriptor_t fd) : fd_(fd) {}

  fd_source_t(fd_source_t&&) = default;

  auto operator=(fd_source_t&& s) -> fd_source_t& = default;

  [[nodiscard]] auto good() -> bool override;
  auto restart() -> void override;

  /**
   * Return true if the buffer is not empty after a non-blocking
   * read.
   */
  [[nodiscard]] auto has_data() -> bool;

  auto skip(size_t len) -> void override;

  [[nodiscard]] auto fd() const -> descriptor_t { return fd_; }
  auto set_fd(descriptor_t fd) -> void { fd_ = fd; }
  [[nodiscard]] auto bytes_read() const -> size_t { return read_; }
  [[nodiscard]] auto end_of_file_error() const -> std::string_view { return *end_of_file_error_; }
  auto set_end_of_file_error(backed_string_view_t error) -> void {
    end_of_file_error_ = std::move(error);
  }
  [[nodiscard]] auto is_seekable() const -> bool { return is_seekable_; }
  auto set_is_seekable(bool seekable) -> void { is_seekable_ = seekable; }

protected:
  auto read_unbuffered(char* data, size_t len) -> size_t override;

private:
  descriptor_t fd_;
  size_t read_ = 0;
  backed_string_view_t end_of_file_error_{"unexpected end-of-file"};
  bool is_seekable_ = true;
  bool good_ = true;
};

/**
 * A sink that writes data to a string.
 */
class string_sink_t : public Sink {
public:
  string_sink_t() = default;

  explicit string_sink_t(const size_t reserved_size) { s_.reserve(reserved_size); }

  explicit string_sink_t(std::string&& s) : s_(std::move(s)) {}

  auto operator()(std::string_view data) -> void override;

  [[nodiscard]] auto str() const -> const std::string& { return s_; }
  [[nodiscard]] auto str() -> std::string& { return s_; }

private:
  std::string s_;
};

/**
 * A source that reads data from a string.
 */
class string_source_t : public restartable_source_t {
public:
  // NOTE: Prevent unintentional dangling views when an implicit conversion
  // from std::string -> std::string_view occurs when the string is passed
  // by rvalue.
  string_source_t(std::string&&) = delete;

  explicit string_source_t(std::string_view s) : s_(s), pos_(0) {}

  explicit string_source_t(const std::string& str) : string_source_t(std::string_view(str)) {}

  auto read(char* data, size_t len) -> size_t override;

  auto skip(size_t len) -> void override;

  auto restart() -> void override { pos_ = 0; }

  [[nodiscard]] auto view() const -> std::string_view { return s_; }
  [[nodiscard]] auto pos() const -> size_t { return pos_; }

private:
  std::string_view s_;
  size_t pos_;
};

/**
 * Compresses a restartable_source_t using the specified compression method.
 *
 * @note currently this buffers the entire compressed data stream in memory. In the future it may
 * instead compress data on demand, lazily pulling from the original `restartable_source_t`. In that
 * case, the `size()` method would go away because we would not in fact know the compressed size in
 * advance.
 */
class compressed_source_t : public restartable_source_t {
public:
  /**
   * Compress a restartable_source_t using the specified compression method.
   *
   * @param source The source data to compress
   * @param compression_method The compression method to use (e.g., "xz", "br")
   */
  compressed_source_t(restartable_source_t& source, const std::string& compression_method);

  auto read(char* data, size_t len) -> size_t override { return string_source_.read(data, len); }

  auto restart() -> void override { string_source_.restart(); }

  [[nodiscard]] auto size() const -> uint64_t { return compressed_data_.size(); }

  [[nodiscard]] auto get_compression_method() const -> std::string_view {
    return compression_method_;
  }

private:
  std::string compressed_data_;
  std::string compression_method_;
  string_source_t string_source_;
};

/**
 * A sink that writes all incoming data to two other sinks.
 */
class tee_sink_t : public Sink {
public:
  tee_sink_t(Sink& sink1, Sink& sink2) : sink1_(sink1), sink2_(sink2) {}

  auto operator()(std::string_view data) -> void override {
    sink1_(data);
    sink2_(data);
  }

private:
  Sink& sink1_;
  Sink& sink2_;
};

/**
 * Adapter class of a Source that saves all data read to a sink.
 */
class tee_source_t : public Source {
public:
  tee_source_t(Source& orig, Sink& sink) : orig_(orig), sink_(sink) {}

  auto read(char* data, size_t len) -> size_t override {
    size_t n = orig_.read(data, len);
    sink_({data, n});
    return n;
  }

private:
  Source& orig_;
  Sink& sink_;
};

/**
 * A reader that consumes the original Source until 'size'.
 */
class sized_source_t : public Source {
public:
  sized_source_t(Source& orig, size_t size) : orig_(orig), remain_(size) {}

  auto read(char* data, size_t len) -> size_t override {
    if (remain_ <= 0) {
      throw EndOfFile("sized: unexpected end-of-file");
    }
    len = std::min(len, remain_);
    size_t n = orig_.read(data, len);
    remain_ -= n;
    return n;
  }

  /**
   * Consume the original source until no remain data is left to consume.
   */
  [[nodiscard]] auto drain_all() -> size_t {
    std::vector<char> buf(8192);
    size_t sum = 0;
    while (remain_ > 0) {
      size_t n = read(buf.data(), buf.size());
      sum += n;
    }
    return sum;
  }

  [[nodiscard]] auto remain() const -> size_t { return remain_; }

private:
  Source& orig_;
  size_t remain_;
};

/**
 * A sink that that just counts the number of bytes given to it
 */
class length_sink_t : public Sink {
public:
  auto operator()(std::string_view data) -> void override { length_ += data.size(); }

  [[nodiscard]] auto length() const -> uint64_t { return length_; }

private:
  uint64_t length_ = 0;
};

/**
 * A wrapper source that counts the number of bytes read from it.
 */
class length_source_t : public Source {
public:
  explicit length_source_t(Source& next) : next_(next) {}

  auto read(char* data, size_t len) -> size_t override {
    auto n = next_.read(data, len);
    total_ += n;
    return n;
  }

  [[nodiscard]] auto total() const -> uint64_t { return total_; }

private:
  Source& next_;
  uint64_t total_ = 0;
};

/**
 * Convert a function into a sink.
 */
class lambda_sink_t : public Sink {
public:
  using data_t = std::function<void(std::string_view data)>;
  using cleanup_t = std::function<void()>;

  lambda_sink_t(
      const data_t& data_fun, const cleanup_t& cleanup_fun = []() {})
      : data_fun_(data_fun), cleanup_fun_(cleanup_fun) {}

  ~lambda_sink_t() override { cleanup_fun_(); }

  auto operator()(std::string_view data) -> void override { data_fun_(data); }

private:
  data_t data_fun_;
  cleanup_t cleanup_fun_;
};

/**
 * Convert a function into a source.
 */
class lambda_source_t : public Source {
public:
  using lambda_t = std::function<size_t(char*, size_t)>;

  explicit lambda_source_t(const lambda_t& lambda) : lambda_(lambda) {}

  auto read(char* data, size_t len) -> size_t override { return lambda_(data, len); }

private:
  lambda_t lambda_;
};

/**
 * Chain two sources together so after the first is exhausted, the second is
 * used
 */
class chain_source_t : public Source {
public:
  chain_source_t(Source& s1, Source& s2) : source1_(s1), source2_(s2) {}

  auto read(char* data, size_t len) -> size_t override;

private:
  Source& source1_;
  Source& source2_;
  bool use_second_ = false;
};

[[nodiscard]] auto source_to_sink(std::function<void(Source&)> fun)
    -> std::unique_ptr<finish_sink_t>;

/**
 * Convert a function that feeds data into a Sink into a Source. The
 * Source executes the function as a coroutine.
 */
[[nodiscard]] auto sink_to_source(
    std::function<void(Sink&)> fun, std::function<void()> eof = []() {
      throw EndOfFile("coroutine has finished");
    }) -> std::unique_ptr<Source>;

auto write_padding(size_t len, Sink& sink) -> void;
auto write_string(std::string_view s, Sink& sink) -> void;

inline auto operator<<(Sink& sink, uint64_t n) -> Sink& {
  unsigned char buf[8];
  buf[0] = n & 0xff;
  buf[1] = (n >> 8) & 0xff;
  buf[2] = (n >> 16) & 0xff;
  buf[3] = (n >> 24) & 0xff;
  buf[4] = (n >> 32) & 0xff;
  buf[5] = (n >> 40) & 0xff;
  buf[6] = (n >> 48) & 0xff;
  buf[7] = (unsigned char)(n >> 56) & 0xff;
  sink({(char*)buf, sizeof(buf)});
  return sink;
}

auto operator<<(Sink& in, const Error& ex) -> Sink&;
auto operator<<(Sink& sink, std::string_view s) -> Sink&;
auto operator<<(Sink& sink, const strings_t& s) -> Sink&;
auto operator<<(Sink& sink, const string_set_t& s) -> Sink&;

make_error(SerialisationError, Error);

template <typename T>
[[nodiscard]] auto read_num(Source& source) -> T {
  unsigned char buf[8];
  source((char*)buf, sizeof(buf));

  auto n = read_little_endian<uint64_t>(buf);

  if (n > (uint64_t)std::numeric_limits<T>::max())
    throw SerialisationError("serialised integer %d is too large for type '%s'", n,
                             typeid(T).name());

  return (T)n;
}

[[nodiscard]] inline auto read_int(Source& source) -> unsigned int {
  return read_num<unsigned int>(source);
}

[[nodiscard]] inline auto read_long_long(Source& source) -> uint64_t {
  return read_num<uint64_t>(source);
}

auto read_padding(size_t len, Source& source) -> void;
[[nodiscard]] auto read_string(char* buf, size_t max, Source& source) -> size_t;
[[nodiscard]] auto read_string(Source& source, size_t max = std::numeric_limits<size_t>::max())
    -> std::string;

template <class T>
[[nodiscard]] auto read_strings(Source& source) -> T;

auto operator>>(Source& in, std::string& s) -> Source&;

template <typename T>
auto operator>>(Source& in, T& n) -> Source& {
  n = read_num<T>(in);
  return in;
}

template <typename T>
auto operator>>(Source& in, bool& b) -> Source& {
  b = read_num<uint64_t>(in);
  return in;
}

[[nodiscard]] auto read_error(Source& source) -> Error;

/**
 * An adapter that converts a std::basic_istream into a source.
 */
class stream_to_source_adapter_t : public Source {
public:
  explicit stream_to_source_adapter_t(std::shared_ptr<std::basic_istream<char>> istream)
      : istream_(std::move(istream)) {}

  auto read(char* data, size_t len) -> size_t override {
    if (!istream_->read(data, len)) {
      if (istream_->eof()) {
        if (istream_->gcount() == 0)
          throw EndOfFile("end of file");
      } else
        throw Error("I/O error in StreamToSourceAdapter");
    }
    return istream_->gcount();
  }

private:
  std::shared_ptr<std::basic_istream<char>> istream_;
};

/**
 * A source that reads a distinct format of concatenated chunks back into its
 * logical form, in order to guarantee a known state to the original stream,
 * even in the event of errors.
 *
 * use with framed_sink_t, which also allows the logical stream to be terminated
 * in the event of an exception.
 */
class framed_source_t : public Source {
public:
  explicit framed_source_t(Source& from) : from_(from) {}

  ~framed_source_t() override {
    try {
      if (!eof_) {
        while (true) {
          auto n = read_int(from_);
          if (!n)
            break;
          std::vector<char> data(n);
          from_(data.data(), n);
        }
      }
    } catch (...) {
      ignore_exception_in_destructor();
    }
  }

  auto read(char* data, size_t len) -> size_t override {
    if (eof_)
      throw EndOfFile("reached end of FramedSource");

    if (pos_ >= pending_.size()) {
      size_t chunk_len = read_int(from_);
      if (!chunk_len) {
        eof_ = true;
        return 0;
      }
      pending_ = std::vector<char>(chunk_len);
      pos_ = 0;
      from_(pending_.data(), chunk_len);
    }

    auto n = std::min(len, pending_.size() - pos_);
    memcpy(data, pending_.data() + pos_, n);
    pos_ += n;
    return n;
  }

private:
  Source& from_;
  bool eof_ = false;
  std::vector<char> pending_;
  size_t pos_ = 0;
};

/**
 * Write as chunks in the format expected by framed_source_t.
 *
 * The `check_error` function can be used to terminate the stream when you
 * detect that an error has occurred. It does so by throwing an exception.
 */
class framed_sink_t : public nix::buffered_sink_t {
public:
  framed_sink_t(buffered_sink_t& to, std::function<void()>&& check_error)
      : to_(to), check_error_(std::move(check_error)) {}

  ~framed_sink_t() override {
    try {
      to_ << 0;
      to_.flush();
    } catch (...) {
      ignore_exception_in_destructor();
    }
  }

  auto write_unbuffered(std::string_view data) -> void override {
    /* Don't send more data if an error has occurred. */
    check_error_();

    to_ << data.size();
    to_(data);
  }

private:
  buffered_sink_t& to_;
  std::function<void()> check_error_;
};

} // namespace nix
