#include "nix/util/serialise.h"

#include <cerrno>
#include <cstring>
#include <memory>

#include <boost/coroutine2/coroutine.hpp>

#include "nix/util/compression.h"
#include "nix/util/signals.h"
#include "nix/util/socket.h"
#include "nix/util/util.h"

#ifdef _WIN32
#  include <fileapi.h>

#  include "nix/util/windows-error.h"
#else
#  include <poll.h>
#endif

namespace nix {

void buffered_sink_t::operator()(std::string_view data) {
  if (!buffer_) {
    buffer_ = decltype(buffer_)(new char[buf_size_]);
  }

  while (!data.empty()) {
    /* Optimisation: bypass the buffer if the data exceeds the
       buffer size. */
    if (buf_pos_ + data.size() >= buf_size_) {
      flush();
      write_unbuffered(data);
      break;
    }
    /* Otherwise, copy the bytes to the buffer.  Flush the buffer
       when it's full. */
    size_t n = buf_pos_ + data.size() > buf_size_ ? buf_size_ - buf_pos_ : data.size();
    memcpy(buffer_.get() + buf_pos_, data.data(), n);
    data.remove_prefix(n);
    buf_pos_ += n;
    if (buf_pos_ == buf_size_) {
      flush();
    }
  }
}

void buffered_sink_t::flush() {
  if (buf_pos_ == 0) {
    return;
  }
  size_t n = buf_pos_;
  buf_pos_ = 0; // don't trigger the assert() in ~BufferedSink()
  write_unbuffered({buffer_.get(), n});
}

fd_sink_t::~fd_sink_t() {
  try {
    flush();
  } catch (...) {
    ignore_exception_in_destructor();
  }
}

void fd_sink_t::write_unbuffered(std::string_view data) {
  written_ += data.size();
  try {
    write_full(fd_, data);
  } catch (SystemError& e) {
    good_ = false;
    throw;
  }
}

bool fd_sink_t::good() {
  return good_;
}

void source_t::operator()(char* data, size_t len) {
  while (len) {
    size_t n = read(data, len);
    data += n;
    len -= n;
  }
}

void source_t::operator()(std::string_view data) {
  (*this)((char*)data.data(), data.size());
}

void source_t::drain_into(sink_t& sink) {
  std::array<char, 8192> buf;
  while (true) {
    try {
      auto n = read(buf.data(), buf.size());
      sink({buf.data(), n});
    } catch (EndOfFile&) {
      break;
    }
  }
}

std::string source_t::drain() {
  string_sink_t s;
  drain_into(s);
  return std::move(s.str());
}

void source_t::skip(size_t len) {
  std::array<char, 8192> buf;
  while (len) {
    auto n = read(buf.data(), std::min(len, buf.size()));
    assert(n <= len);
    len -= n;
  }
}

size_t buffered_source_t::read(char* data, size_t len) {
  if (!buffer_) {
    buffer_ = decltype(buffer_)(new char[buf_size_]);
  }

  if (!buf_pos_in_) {
    buf_pos_in_ = read_unbuffered(buffer_.get(), buf_size_);
  }

  /* Copy out the data in the buffer. */
  auto n = std::min(len, buf_pos_in_ - buf_pos_out_);
  memcpy(data, buffer_.get() + buf_pos_out_, n);
  buf_pos_out_ += n;
  if (buf_pos_in_ == buf_pos_out_) {
    buf_pos_in_ = buf_pos_out_ = 0;
  }
  return n;
}

bool buffered_source_t::has_data() {
  return buf_pos_out_ < buf_pos_in_;
}

size_t fd_source_t::read_unbuffered(char* data, size_t len) {
#ifdef _WIN32
  DWORD n;
  check_interrupt();
  if (!::ReadFile(fd_, data, len, &n, NULL)) {
    good_ = false;
    throw windows::WinError("ReadFile when FdSource::readUnbuffered");
  }
#else
  ssize_t n;
  while (true) {
    check_interrupt();
    n = ::read(fd_, data, len);
    if (n == -1) {
      if (errno == EINTR) {
        continue;
      }
      // Handle EAGAIN/EWOULDBLOCK: poll until data is available.
      // This can happen on macOS and other BSD-like systems even on
      // blocking file descriptors in certain edge cases, and also when
      // the fd is inadvertently set to non-blocking mode (e.g. buildhook).
      if (errno == EAGAIN || errno == EWOULDBLOCK) {
        struct pollfd pfd;
        pfd.fd = fd_;
        pfd.events = POLLIN;
        if (poll(&pfd, 1, -1) == -1 && errno != EINTR) {
          good_ = false;
          throw sys_error_t("poll on file descriptor failed");
        }
        continue;
      }
      good_ = false;
      throw sys_error_t("reading from file");
    }
    if (n == 0) {
      good_ = false;
      throw EndOfFile(std::string(*end_of_file_error_));
    }
    break;
  }
#endif
  read_ += n;
  return n;
}

bool fd_source_t::good() {
  return good_;
}

bool fd_source_t::has_data() {
  if (buffered_source_t::has_data()) {
    return true;
  }

  while (true) {
    fd_set fds;
    FD_ZERO(&fds);
    socket_t sock = to_socket(fd_);
    FD_SET(sock, &fds);

    struct timeval timeout;
    timeout.tv_sec = 0;
    timeout.tv_usec = 0;

    auto n = select(sock + 1, &fds, nullptr, nullptr, &timeout);
    if (n < 0) {
      if (errno == EINTR) {
        continue;
      }
      throw sys_error_t("polling file descriptor");
    }
    return FD_ISSET(sock, &fds);
  }
}

void fd_source_t::restart() {
  if (!is_seekable_) {
    throw Error("can't seek to the start of a file");
  }
  buffer_.reset();
  read_ = buf_pos_in_ = buf_pos_out_ = 0;
  int fd_local = from_descriptor_read_only(fd_);
  if (lseek(fd_local, 0, SEEK_SET) == -1) {
    throw sys_error_t("seeking to the start of a file");
  }
}

void fd_source_t::skip(size_t len) {
  /* Discard data in the buffer. */
  if (len && buffer_ && buf_pos_in_ - buf_pos_out_) {
    if (len >= buf_pos_in_ - buf_pos_out_) {
      len -= buf_pos_in_ - buf_pos_out_;
      buf_pos_in_ = buf_pos_out_ = 0;
    } else {
      buf_pos_out_ += len;
      len = 0;
    }
  }

#ifndef _WIN32
  /* If we can, seek forward in the file to skip the rest. */
  if (is_seekable_ && len) {
    if (lseek(fd_, len, SEEK_CUR) == -1) {
      if (errno == ESPIPE) {
        is_seekable_ = false;
      } else {
        throw sys_error_t("seeking forward in file");
      }
    } else {
      read_ += len;
      return;
    }
  }
#endif

  /* Otherwise, skip by reading. */
  if (len) {
    buffered_source_t::skip(len);
  }
}

size_t string_source_t::read(char* data, size_t len) {
  if (pos_ == s_.size()) {
    throw EndOfFile("end of string reached");
  }
  size_t n = s_.copy(data, len, pos_);
  pos_ += n;
  return n;
}

void string_source_t::skip(size_t len) {
  const size_t remain = s_.size() - pos_;
  if (len > remain) {
    pos_ = s_.size();
    throw EndOfFile("end of string reached");
  }
  pos_ += len;
}

compressed_source_t::compressed_source_t(restartable_source_t& source,
                                         const std::string& compression_method)
    : compressed_data_([&]() {
        string_sink_t sink;
        auto compression_sink = make_compression_sink(compression_method, sink);
        source.drain_into(*compression_sink);
        compression_sink->finish();
        return std::move(sink.str());
      }()),
      compression_method_(compression_method),
      string_source_(compressed_data_) {}

std::unique_ptr<finish_sink_t> source_to_sink(std::function<void(source_t&)> fun) {
  struct source_to_sink_t : finish_sink_t {
    typedef boost::coroutines2::coroutine<bool> coro_t;

    std::function<void(source_t&)> fun;
    std::optional<coro_t::push_type> coro;

    source_to_sink_t(std::function<void(source_t&)> fun) : fun(fun) {}

    std::string_view cur;

    void operator()(std::string_view in) override {
      if (in.empty()) {
        return;
      }
      cur = in;

      if (!coro) {
        coro = coro_t::push_type([&](coro_t::pull_type& yield) {
          lambda_source_t source([&](char* out, size_t out_len) {
            if (cur.empty()) {
              yield();
              if (yield.get()) {
                throw EndOfFile("coroutine has finished");
              }
            }

            size_t n = cur.copy(out, out_len);
            cur.remove_prefix(n);
            return n;
          });
          fun(source);
        });
      }

      if (!*coro) {
        unreachable();
      }

      if (!cur.empty()) {
        (*coro)(false);
      }
    }

    void finish() override {
      if (coro && *coro) {
        (*coro)(true);
      }
    }
  };

  return std::make_unique<source_to_sink_t>(fun);
}

std::unique_ptr<source_t> sink_to_source(std::function<void(sink_t&)> fun,
                                         std::function<void()> eof) {
  struct sink_to_source_t : source_t {
    typedef boost::coroutines2::coroutine<std::string_view> coro_t;

    std::function<void(sink_t&)> fun;
    std::function<void()> eof;
    std::optional<coro_t::pull_type> coro;

    sink_to_source_t(std::function<void(sink_t&)> fun, std::function<void()> eof)
        : fun(fun), eof(eof) {}

    std::string_view cur;

    size_t read(char* data, size_t len) override {
      bool has_coro = coro.has_value();
      if (!has_coro) {
        coro = coro_t::pull_type([&](coro_t::push_type& yield) {
          lambda_sink_t sink([&](std::string_view data) {
            if (!data.empty()) {
              yield(data);
            }
          });
          fun(sink);
        });
      }

      if (cur.empty()) {
        if (has_coro) {
          (*coro)();
        }
        if (*coro) {
          cur = coro->get();
        } else {
          coro.reset();
          eof();
          unreachable();
        }
      }

      size_t n = cur.copy(data, len);
      cur.remove_prefix(n);

      return n;
    }
  };

  return std::make_unique<sink_to_source_t>(fun, eof);
}

void write_padding(size_t len, sink_t& sink) {
  if (len % 8) {
    char zero[8];
    memset(zero, 0, sizeof(zero));
    sink({zero, 8 - (len % 8)});
  }
}

void write_string(std::string_view data, sink_t& sink) {
  sink << data.size();
  sink(data);
  write_padding(data.size(), sink);
}

sink_t& operator<<(sink_t& sink, std::string_view s) {
  write_string(s, sink);
  return sink;
}

template <class T>
void write_strings(const T& ss, sink_t& sink) {
  sink << ss.size();
  for (auto& i : ss) {
    sink << i;
  }
}

sink_t& operator<<(sink_t& sink, const strings_t& s) {
  write_strings(s, sink);
  return sink;
}

sink_t& operator<<(sink_t& sink, const string_set_t& s) {
  write_strings(s, sink);
  return sink;
}

sink_t& operator<<(sink_t& sink, const Error& ex) {
  auto& info = ex.info();
  sink << "Error" << static_cast<uint64_t>(info.level_) << "Error" // removed
       << info.msg_.str() << 0                                     // FIXME: info.errPos
       << info.traces_.size();
  for (auto& trace : info.traces_) {
    sink << 0; // FIXME: trace.pos
    sink << trace.hint_.str();
  }
  return sink;
}

void read_padding(size_t len, source_t& source) {
  if (len % 8) {
    char zero[8];
    size_t n = 8 - (len % 8);
    source(zero, n);
    for (unsigned int i = 0; i < n; i++) {
      if (zero[i]) {
        throw SerialisationError("non-zero padding");
      }
    }
  }
}

size_t read_string(char* buf, size_t max, source_t& source) {
  auto len = read_num<size_t>(source);
  if (len > max) {
    throw SerialisationError("string is too long");
  }
  source(buf, len);
  read_padding(len, source);
  return len;
}

std::string read_string(source_t& source, size_t max) {
  auto len = read_num<size_t>(source);
  if (len > max) {
    throw SerialisationError("string is too long");
  }
  std::string res(len, 0);
  source(res.data(), len);
  read_padding(len, source);
  return res;
}

source_t& operator>>(source_t& in, std::string& s) {
  s = read_string(in);
  return in;
}

template <class T>
T read_strings(source_t& source) {
  auto count = read_num<size_t>(source);
  T ss;
  while (count--) {
    ss.insert(ss.end(), read_string(source));
  }
  return ss;
}

template Paths read_strings(source_t& source);
template path_set_t read_strings(source_t& source);

Error read_error(source_t& source) {
  auto type = read_string(source);
  if (type != "Error") {
    throw SerialisationError("expected error type 'Error', got '%s'", type);
  }
  auto level = (verbosity_t)read_int(source);
  [[maybe_unused]] auto name = read_string(source); // removed
  auto msg = read_string(source);
  // NOLINTNEXTLINE(cppcoreguidelines-pro-type-member-init)
  error_info_t info{
      .level_ = level,
      .msg_ = hint_fmt_t(msg),
  };
  auto have_pos = read_num<size_t>(source);
  if (have_pos != 0) {
    throw SerialisationError("expected have_pos == 0, got %d", have_pos);
  }
  auto nr_traces = read_num<size_t>(source);
  for (size_t i = 0; i < nr_traces; ++i) {
    have_pos = read_num<size_t>(source);
    if (have_pos != 0) {
      throw SerialisationError("expected trace have_pos == 0, got %d", have_pos);
    }
    info.traces_.push_back(trace_t{.hint_ = hint_fmt_t(read_string(source))});
  }
  return Error(std::move(info));
}

void string_sink_t::operator()(std::string_view data) {
  s_.append(data);
}

size_t chain_source_t::read(char* data, size_t len) {
  if (use_second_) {
    return source2_.read(data, len);
  } else {
    try {
      return source1_.read(data, len);
    } catch (EndOfFile&) {
      use_second_ = true;
      return this->read(data, len);
    }
  }
}

} // namespace nix
