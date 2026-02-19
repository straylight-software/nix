#pragma once
///@file

#include <string>

#include "nix/util/ref.h"
#include "nix/util/serialise.h"
#include "nix/util/types.h"

namespace nix {

struct compression_sink_t : buffered_sink_t, finish_sink_t {
  using buffered_sink_t::operator();
  using buffered_sink_t::writeUnbuffered;
  using finish_sink_t::finish;
};

std::string decompress(const std::string& method, std::string_view in);

std::unique_ptr<finish_sink_t> makeDecompressionSink(const std::string& method, Sink& nextSink);

std::string compress(const std::string& method, std::string_view in, const bool parallel = false,
                     int level = -1);

ref<compression_sink_t> makeCompressionSink(const std::string& method, Sink& nextSink,
                                         const bool parallel = false, int level = -1);

MakeError(UnknownCompressionMethod, Error);

MakeError(CompressionError, Error);

} // namespace nix
