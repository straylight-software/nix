#pragma once
///@file

#include <string>

#include "nix/util/ref.h"
#include "nix/util/serialise.h"
#include "nix/util/types.h"

namespace nix {

struct compression_sink_t : buffered_sink_t, finish_sink_t {
  using buffered_sink_t::operator();
  using buffered_sink_t::write_unbuffered;
  using finish_sink_t::finish;
};

std::string decompress(const std::string& method, std::string_view in);

std::unique_ptr<finish_sink_t> make_decompression_sink(const std::string& method, Sink& next_sink);

std::string compress(const std::string& method, std::string_view in, const bool parallel = false,
                     int level = -1);

ref<compression_sink_t> make_compression_sink(const std::string& method, Sink& next_sink,
                                         const bool parallel = false, int level = -1);

make_error(UnknownCompressionMethod, Error);

make_error(CompressionError, Error);

} // namespace nix
