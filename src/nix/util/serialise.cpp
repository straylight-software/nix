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
  if (!buffer) {
    buffer = decltype(buffer)(new char[buf_size]);
}

  while (!data.empty()) {
    /* Optimisation: bypass the buffer if the data exceeds the
       buffer size. */
    if (buf_pos + data.size() >= buf_size) {
      flush();
      write_unbuffered(data);
      break;
    }
    /* Otherwise, copy the bytes to the buffer.  Flush the buffer
       when it's full. */
    size_t n = buf_pos + data.size() > buf_size ? buf_size - buf_pos : data.size();
    memcpy(buffer.get() + buf_pos, data.data(), n);
    data.remove_prefix(n);
    buf_pos += n;
    if (buf_pos == buf_size) {
      flush();
}
  }
}

void buffered_sink_t::flush() {
  if (buf_pos == 0) {
    return;
}
  size_t n = buf_pos;
  buf_pos = 0; // don't trigger the assert() in ~BufferedSink()
  write_unbuffered({buffer.get(), n});
}

fd_sink_t::~fd_sink_t() {
  try {
    flush();
  } catch (...) {
    ignore_exception_in_destructor();
  }
}

void fd_sink_t::write_unbuffered(std::string_view data) {
  written += data.size();
  try {
    write_full(fd, data);
  } catch (SystemError& e) {
    _good = false;
    throw;
  }
}

bool fd_sink_t::good() {
  return _good;
}

void Source::operator()(char* data, size_t len) {
  while (len) {
    size_t n = read(data, len);
    data += n;
    len -= n;
  }
}

void Source::operator()(std::string_view data) {
  (*this)((char*)data.data(), data.size());
}

void Source::drain_into(Sink& sink) {
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

std::string Source::drain() {
  string_sink_t s;
  drain_into(s);
  return std::move(s.s);
}

void Source::skip(size_t len) {
  std::array<char, 8192> buf;
  while (len) {
    auto n = read(buf.data(), std::min(len, buf.size()));
    assert(n <= len);
    len -= n;
  }
}

size_t buffered_source_t::read(char* data, size_t len) {
  if (!buffer) {
    buffer = decltype(buffer)(new char[buf_size]);
}

  if (!buf_pos_in) {
    buf_pos_in = read_unbuffered(buffer.get(), buf_size);
}

  /* Copy out the data in the buffer. */
  auto n = std::min(len, buf_pos_in - buf_pos_out);
  memcpy(data, buffer.get() + buf_pos_out, n);
  buf_pos_out += n;
  if (buf_pos_in == buf_pos_out) {
    buf_pos_in = buf_pos_out = 0;
}
  return n;
}

bool buffered_source_t::has_data() {
  return buf_pos_out < buf_pos_in;
}

size_t fd_source_t::read_unbuffered(char* data, size_t len) {
#ifdef _WIN32
  DWORD n;
  check_interrupt();
  if (!::ReadFile(fd, data, len, &n, NULL)) {
    _good = false;
    throw windows::WinError("ReadFile when FdSource::readUnbuffered");
  }
#else
  ssize_t n;
  do {
    check_interrupt();
    n = ::read(fd, data, len);
  } while (n == -1 && errno == EINTR);
  if (n == -1) {
    _good = false;
    throw sys_error_t("reading from file");
  }
  if (n == 0) {
    _good = false;
    throw EndOfFile(std::string(*end_of_file_error));
  }
#endif
  read += n;
  return n;
}

bool fd_source_t::good() {
  return _good;
}

bool fd_source_t::has_data() {
  if (buffered_source_t::has_data()) {
    return true;
}

  while (true) {
    fd_set fds;
    FD_ZERO(&fds);
    socket_t sock = to_socket(fd);
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
  if (!is_seekable) {
    throw Error("can't seek to the start of a file");
}
  buffer.reset();
  read = buf_pos_in = buf_pos_out = 0;
  int fd_ = from_descriptor_read_only(fd);
  if (lseek(fd_, 0, SEEK_SET) == -1) {
    throw sys_error_t("seeking to the start of a file");
}
}

void fd_source_t::skip(size_t len) {
  /* Discard data in the buffer. */
  if (len && buffer && buf_pos_in - buf_pos_out) {
    if (len >= buf_pos_in - buf_pos_out) {
      len -= buf_pos_in - buf_pos_out;
      buf_pos_in = buf_pos_out = 0;
    } else {
      buf_pos_out += len;
      len = 0;
    }
  }

#ifndef _WIN32
  /* If we can, seek forward in the file to skip the rest. */
  if (is_seekable && len) {
    if (lseek(fd, len, SEEK_CUR) == -1) {
      if (errno == ESPIPE) {
        is_seekable = false;
      } else {
        throw sys_error_t("seeking forward in file");
}
    } else {
      read += len;
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
  if (pos == s.size()) {
    throw EndOfFile("end of string reached");
}
  size_t n = s.copy(data, len, pos);
  pos += n;
  return n;
}

void string_source_t::skip(size_t len) {
  const size_t remain = s.size() - pos;
  if (len > remain) {
    pos = s.size();
    throw EndOfFile("end of string reached");
  }
  pos += len;
}

compressed_source_t::compressed_source_t(restartable_source_t& source, const std::string& compression_method)
    : compressedData([&]() {
        string_sink_t sink;
        auto compression_sink = make_compression_sink(compression_method, sink);
        source.drain_into(*compression_sink);
        compression_sink->finish();
        return std::move(sink.s);
      }()),
      compression_method(compression_method),
      stringSource(compressedData) {}

std::unique_ptr<finish_sink_t> source_to_sink(std::function<void(Source&)> fun) {
  struct source_to_sink_t : finish_sink_t {
    typedef boost::coroutines2::coroutine<bool> coro_t;

    std::function<void(Source&)> fun;
    std::optional<coro_t::push_type> coro;

    source_to_sink_t(std::function<void(Source&)> fun) : fun(fun) {}

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

std::unique_ptr<Source> sink_to_source(std::function<void(Sink&)> fun, std::function<void()> eof) {
  struct sink_to_source_t : Source {
    typedef boost::coroutines2::coroutine<std::string_view> coro_t;

    std::function<void(Sink&)> fun;
    std::function<void()> eof;
    std::optional<coro_t::pull_type> coro;

    sink_to_source_t(std::function<void(Sink&)> fun, std::function<void()> eof) : fun(fun), eof(eof) {}

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

void write_padding(size_t len, Sink& sink) {
  if (len % 8) {
    char zero[8];
    memset(zero, 0, sizeof(zero));
    sink({zero, 8 - (len % 8)});
  }
}

void write_string(std::string_view data, Sink& sink) {
  sink << data.size();
  sink(data);
  write_padding(data.size(), sink);
}

Sink& operator<<(Sink& sink, std::string_view s) {
  write_string(s, sink);
  return sink;
}

template <class T>
void write_strings(const T& ss, Sink& sink) {
  sink << ss.size();
  for (auto& i : ss) {
    sink << i;
}
}

Sink& operator<<(Sink& sink, const strings_t& s) {
  write_strings(s, sink);
  return sink;
}

Sink& operator<<(Sink& sink, const string_set_t& s) {
  write_strings(s, sink);
  return sink;
}

Sink& operator<<(Sink& sink, const Error& ex) {
  auto& info = ex.info();
  sink << "Error" << info.level << "Error" // removed
       << info.msg.str() << 0              // FIXME: info.errPos
       << info.traces.size();
  for (auto& trace : info.traces) {
    sink << 0; // FIXME: trace.pos
    sink << trace.hint.str();
  }
  return sink;
}

void read_padding(size_t len, Source& source) {
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

size_t read_string(char* buf, size_t max, Source& source) {
  auto len = read_num<size_t>(source);
  if (len > max) {
    throw SerialisationError("string is too long");
}
  source(buf, len);
  read_padding(len, source);
  return len;
}

std::string read_string(Source& source, size_t max) {
  auto len = read_num<size_t>(source);
  if (len > max) {
    throw SerialisationError("string is too long");
}
  std::string res(len, 0);
  source(res.data(), len);
  read_padding(len, source);
  return res;
}

Source& operator>>(Source& in, std::string& s) {
  s = read_string(in);
  return in;
}

template <class T>
T read_strings(Source& source) {
  auto count = read_num<size_t>(source);
  T ss;
  while (count--) {
    ss.insert(ss.end(), read_string(source));
}
  return ss;
}

template Paths read_strings(Source& source);
template path_set_t read_strings(Source& source);

Error read_error(Source& source) {
  auto type = read_string(source);
  assert(type == "Error");
  auto level = (verbosity_t)read_int(source);
  [[maybe_unused]] auto name = read_string(source); // removed
  auto msg = read_string(source);
  error_info_t info{
      .level = level,
      .msg = hint_fmt_t(msg),
  };
  auto have_pos = read_num<size_t>(source);
  assert(have_pos == 0);
  auto nr_traces = read_num<size_t>(source);
  for (size_t i = 0; i < nr_traces; ++i) {
    have_pos = read_num<size_t>(source);
    assert(have_pos == 0);
    info.traces.push_back(trace_t{.hint = hint_fmt_t(read_string(source))});
  }
  return Error(std::move(info));
}

void string_sink_t::operator()(std::string_view data) {
  s.append(data);
}

size_t chain_source_t::read(char* data, size_t len) {
  if (use_second) {
    return source2.read(data, len);
  } else {
    try {
      return source1.read(data, len);
    } catch (EndOfFile&) {
      use_second = true;
      return this->read(data, len);
    }
  }
}

} // namespace nix
