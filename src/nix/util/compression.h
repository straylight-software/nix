#pragma once
///@file

#include <string>

#include "nix/util/ref.h"
#include "nix/util/serialise.h"
#include "nix/util/types.h"

namespace nix {

struct CompressionSink : BufferedSink, FinishSink {
  using BufferedSink::operator();
  using BufferedSink::writeUnbuffered;
  using FinishSink::finish;
};

std::string decompress(const std::string& method, std::string_view in);

std::unique_ptr<FinishSink> makeDecompressionSink(const std::string& method, Sink& nextSink);

std::string compress(const std::string& method, std::string_view in, const bool parallel = false,
                     int level = -1);

ref<CompressionSink> makeCompressionSink(const std::string& method, Sink& nextSink,
                                         const bool parallel = false, int level = -1);

MakeError(UnknownCompressionMethod, Error);

MakeError(CompressionError, Error);

} // namespace nix
