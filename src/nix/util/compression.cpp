#include "nix/util/compression.h"

#include <cstdio>
#include <cstring>

#include <archive.h>
#include <archive_entry.h>
#include <brotli/decode.h>
#include <brotli/encode.h>

#include "nix/util/finally.h"
#include "nix/util/logging.h"
#include "nix/util/signals.h"
#include "nix/util/tarfile.h"

namespace nix {

static const int COMPRESSION_LEVEL_DEFAULT = -1;

// Don't feed brotli too much at once.
struct chunked_compression_sink_t : compression_sink_t {
  uint8_t outbuf[32 * 1024];

  void write_unbuffered(std::string_view data) override {
    const size_t CHUNK_SIZE = sizeof(outbuf) << 2;
    while (!data.empty()) {
      size_t n = std::min(CHUNK_SIZE, data.size());
      write_internal(data.substr(0, n));
      data.remove_prefix(n);
    }
  }

  virtual void write_internal(std::string_view data) = 0;
};

struct archive_decompression_source_t : Source {
  std::unique_ptr<tar_archive_t> archive = 0;
  Source& src;
  std::optional<std::string> compression_method;

  archive_decompression_source_t(Source& src,
                             std::optional<std::string> compression_method = std::nullopt)
      : src(src), compression_method(std::move(compression_method)) {}

  ~archive_decompression_source_t() override {}

  size_t read(char* data, size_t len) override {
    struct archive_entry* ae;
    if (!archive) {
      archive = std::make_unique<tar_archive_t>(src, /*raw*/ true, compression_method);
      this->archive->check(archive_read_next_header(this->archive->archive, &ae),
                           "failed to read header (%s)");
      if (archive_filter_count(this->archive->archive) < 2) {
        throw CompressionError("input compression not recognized");
      }
    }
    ssize_t result = archive_read_data(this->archive->archive, data, len);
    if (result > 0) {
      return result;
}
    if (result == 0) {
      throw EndOfFile("reached end of compressed file");
    }
    this->archive->check(result, "failed to read compressed data (%s)");
    return result;
  }
};

struct archive_compression_sink_t : compression_sink_t {
  Sink& next_sink;
  struct archive* archive;

  archive_compression_sink_t(Sink& next_sink, std::string format, bool parallel,
                         int level = COMPRESSION_LEVEL_DEFAULT)
      : next_sink(next_sink) {
    archive = archive_write_new();
    if (!archive) {
      throw Error("failed to initialize libarchive");
}
    check(archive_write_add_filter_by_name(archive, format.c_str()),
          "couldn't initialize compression (%s)");
    check(archive_write_set_format_raw(archive));
    if (parallel) {
      check(archive_write_set_filter_option(archive, format.c_str(), "threads", "0"));
}
    if (level != COMPRESSION_LEVEL_DEFAULT) {
      check(archive_write_set_filter_option(archive, format.c_str(), "compression-level",
                                            std::to_string(level).c_str()));
}
    // disable internal buffering
    check(archive_write_set_bytes_per_block(archive, 0));
    // disable output padding
    check(archive_write_set_bytes_in_last_block(archive, 1));
    open();
  }

  ~archive_compression_sink_t() override {
    if (archive) {
      archive_write_free(archive);
}
  }

  void finish() override {
    flush();
    check(archive_write_close(archive));
  }

  void check(int err, const std::string& reason = "failed to compress (%s)") {
    if (err == ARCHIVE_EOF) {
      throw EndOfFile("reached end of archive");
    } else if (err != ARCHIVE_OK) {
      throw Error(reason, archive_error_string(this->archive));
}
  }

  void write_unbuffered(std::string_view data) override {
    ssize_t result = archive_write_data(archive, data.data(), data.length());
    if (result <= 0) {
      check(result);
}
  }

private:
  void open() {
    check(archive_write_open(archive, this, nullptr, archive_compression_sink_t::callback_write,
                             nullptr));
    auto ae = archive_entry_new();
    archive_entry_set_filetype(ae, AE_IFREG);
    check(archive_write_header(archive, ae));
    archive_entry_free(ae);
  }

  static ssize_t callback_write(struct archive* archive, void* _self, const void* buffer,
                                size_t length) {
    auto self = (archive_compression_sink_t*)_self;
    self->next_sink({(const char*)buffer, length});
    return length;
  }
};

struct none_sink_t : compression_sink_t {
  Sink& next_sink;

  none_sink_t(Sink& next_sink, int level = COMPRESSION_LEVEL_DEFAULT) : next_sink(next_sink) {
    if (level != COMPRESSION_LEVEL_DEFAULT) {
      warn("requested compression level '%d' not supported by compression method 'none'", level);
}
  }

  void finish() override { flush(); }

  void write_unbuffered(std::string_view data) override { next_sink(data); }
};

struct brotli_decompression_sink_t : chunked_compression_sink_t {
  Sink& next_sink;
  BrotliDecoderState* state;
  bool finished = false;

  brotli_decompression_sink_t(Sink& next_sink) : next_sink(next_sink) {
    state = BrotliDecoderCreateInstance(nullptr, nullptr, nullptr);
    if (!state) {
      throw CompressionError("unable to initialize brotli decoder");
}
  }

  ~brotli_decompression_sink_t() { BrotliDecoderDestroyInstance(state); }

  void finish() override {
    flush();
    write_internal({});
  }

  void write_internal(std::string_view data) override {
    auto next_in = (const uint8_t*)data.data();
    size_t avail_in = data.size();
    uint8_t* next_out = outbuf;
    size_t avail_out = sizeof(outbuf);

    while (!finished && (!data.data() || avail_in)) {
      check_interrupt();

      if (!BrotliDecoderDecompressStream(state, &avail_in, &next_in, &avail_out, &next_out,
                                         nullptr)) {
        throw CompressionError("error while decompressing brotli file");
}

      if (avail_out < sizeof(outbuf) || avail_in == 0) {
        next_sink({(char*)outbuf, sizeof(outbuf) - avail_out});
        next_out = outbuf;
        avail_out = sizeof(outbuf);
      }

      finished = BrotliDecoderIsFinished(state);
    }
  }
};

std::string decompress(const std::string& method, std::string_view in) {
  string_sink_t ssink;
  auto sink = make_decompression_sink(method, ssink);
  (*sink)(in);
  sink->finish();
  return std::move(ssink.s);
}

std::unique_ptr<finish_sink_t> make_decompression_sink(const std::string& method, Sink& next_sink) {
  if (method == "none" || method == "" || method == "identity") {
    return std::make_unique<none_sink_t>(next_sink);
  } else if (method == "br") {
    return std::make_unique<brotli_decompression_sink_t>(next_sink);
  } else {
    return source_to_sink([method, &next_sink](Source& source) {
      auto decompression_source = std::make_unique<archive_decompression_source_t>(source, method);
      decompression_source->drain_into(next_sink);
    });
}
}

struct brotli_compression_sink_t : chunked_compression_sink_t {
  Sink& next_sink;
  uint8_t outbuf[BUFSIZ];
  BrotliEncoderState* state;
  bool finished = false;

  brotli_compression_sink_t(Sink& next_sink) : next_sink(next_sink) {
    state = BrotliEncoderCreateInstance(nullptr, nullptr, nullptr);
    if (!state) {
      throw CompressionError("unable to initialise brotli encoder");
}
  }

  ~brotli_compression_sink_t() { BrotliEncoderDestroyInstance(state); }

  void finish() override {
    flush();
    write_internal({});
  }

  void write_internal(std::string_view data) override {
    auto next_in = (const uint8_t*)data.data();
    size_t avail_in = data.size();
    uint8_t* next_out = outbuf;
    size_t avail_out = sizeof(outbuf);

    while (!finished && (!data.data() || avail_in)) {
      check_interrupt();

      if (!BrotliEncoderCompressStream(
              state, data.data() ? BROTLI_OPERATION_PROCESS : BROTLI_OPERATION_FINISH, &avail_in,
              &next_in, &avail_out, &next_out, nullptr)) {
        throw CompressionError("error while compressing brotli compression");
}

      if (avail_out < sizeof(outbuf) || avail_in == 0) {
        next_sink({(const char*)outbuf, sizeof(outbuf) - avail_out});
        next_out = outbuf;
        avail_out = sizeof(outbuf);
      }

      finished = BrotliEncoderIsFinished(state);
    }
  }
};

ref<compression_sink_t> make_compression_sink(const std::string& method, Sink& next_sink,
                                         const bool parallel, int level) {
  std::vector<std::string> la_supports = {"bzip2", "compress", "grzip", "gzip", "lrzip", "lz4",
                                          "lzip",  "lzma",     "lzop",  "xz",   "zstd"};
  if (std::find(la_supports.begin(), la_supports.end(), method) != la_supports.end()) {
    return make_ref<archive_compression_sink_t>(next_sink, method, parallel, level);
  }
  if (method == "none") {
    return make_ref<none_sink_t>(next_sink);
  } else if (method == "br") {
    return make_ref<brotli_compression_sink_t>(next_sink);
  } else {
    throw UnknownCompressionMethod("unknown compression method '%s'", method);
}
}

std::string compress(const std::string& method, std::string_view in, const bool parallel,
                     int level) {
  string_sink_t ssink;
  auto sink = make_compression_sink(method, ssink, parallel, level);
  (*sink)(in);
  sink->finish();
  return std::move(ssink.s);
}

} // namespace nix
