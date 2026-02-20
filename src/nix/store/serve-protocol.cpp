#include "nix/store/serve-protocol.h"

#include <nlohmann/json.hpp>

#include "nix/store/build-result.h"
#include "nix/store/path-info.h"
#include "nix/store/path-with-outputs.h"
#include "nix/store/serve-protocol-impl.h"
#include "nix/store/store-api.h"
#include "nix/util/archive.h"
#include "nix/util/serialise.h"

namespace nix {

/* protocol-specific definitions */

build_result_t ServeProto::Serialise<build_result_t>::read(const store_dir_config_t& store,
                                                     ServeProto::ReadConn conn) {
  build_result_t status;
  build_result_t::Success success;
  build_result_t::Failure failure;

  auto rawStatus = read_int(conn.from);
  conn.from >> failure.errorMsg;

  if (GET_PROTOCOL_MINOR(conn.version) >= 3)
    conn.from >> status.timesBuilt >> failure.isNonDeterministic >> status.start_time >>
        status.stopTime;
  if (GET_PROTOCOL_MINOR(conn.version) >= 6) {
    auto built_outputs = ServeProto::Serialise<DrvOutputs>::read(store, conn);
    for (auto&& [output, realisation] : built_outputs)
      success.built_outputs.insert_or_assign(std::move(output.output_name), std::move(realisation));
  }

  if (build_result_t::Success::statusIs(rawStatus)) {
    success.status = static_cast<build_result_t::Success::Status>(rawStatus);
    status.inner = std::move(success);
  } else {
    failure.status = static_cast<build_result_t::Failure::Status>(rawStatus);
    status.inner = std::move(failure);
  }

  return status;
}

void ServeProto::Serialise<build_result_t>::write(const store_dir_config_t& store,
                                               ServeProto::WriteConn conn, const build_result_t& res) {
  /* The protocol predates the use of sum types (std::variant) to
     separate the success or failure cases. As such, it transits some
     success- or failure-only fields in both cases. This helper
     function helps support this: in each case, we just pass the old
     default value for the fields that don't exist in that case. */
  auto common = [&](std::string_view errorMsg, bool isNonDeterministic, const auto& built_outputs) {
    conn.to << errorMsg;
    if (GET_PROTOCOL_MINOR(conn.version) >= 3)
      conn.to << res.timesBuilt << isNonDeterministic << res.start_time << res.stopTime;
    if (GET_PROTOCOL_MINOR(conn.version) >= 6) {
      DrvOutputs builtOutputsFullKey;
      for (auto& [output, realisation] : built_outputs)
        builtOutputsFullKey.insert_or_assign(realisation.id, realisation);
      ServeProto::write(store, conn, builtOutputsFullKey);
    }
  };
  std::visit(overloaded{
                 [&](const build_result_t::Failure& failure) {
                   conn.to << failure.status;
                   common(failure.errorMsg, failure.isNonDeterministic,
                          decltype(build_result_t::Success::built_outputs){});
                 },
                 [&](const build_result_t::Success& success) {
                   conn.to << success.status;
                   common(/*errorMsg=*/"", /*isNonDeterministic=*/false, success.built_outputs);
                 },
             },
             res.inner);
}

UnkeyedValidPathInfo ServeProto::Serialise<UnkeyedValidPathInfo>::read(const store_dir_config_t& store,
                                                                       ReadConn conn) {
  /* Hash should be set below unless very old `nix-store --serve`.
     Caller should assert that it did set it. */
  UnkeyedValidPathInfo info{store, Hash::dummy};

  auto deriver = read_string(conn.from);
  if (deriver != "")
    info.deriver = store.parseStorePath(deriver);
  info.references = ServeProto::Serialise<store_path_set_t>::read(store, conn);

  read_long_long(conn.from); // download size, unused
  info.nar_size = read_long_long(conn.from);

  if (GET_PROTOCOL_MINOR(conn.version) >= 4) {
    auto s = read_string(conn.from);
    if (!s.empty())
      info.nar_hash = Hash::parse_any_prefixed(s);
    info.ca = content_address_t::parseOpt(read_string(conn.from));
    info.sigs = read_strings<string_set_t>(conn.from);
  }

  return info;
}

void ServeProto::Serialise<UnkeyedValidPathInfo>::write(const store_dir_config_t& store, WriteConn conn,
                                                        const UnkeyedValidPathInfo& info) {
  conn.to << (info.deriver ? store.printStorePath(*info.deriver) : "");

  ServeProto::write(store, conn, info.references);
  // !!! Maybe we want compression?
  conn.to << info.nar_size // downloadSize, lie a little
          << info.nar_size;
  if (GET_PROTOCOL_MINOR(conn.version) >= 4)
    conn.to << info.nar_hash.to_string(hash_format_t::nix32, true) << render_content_address(info.ca)
            << info.sigs;
}

ServeProto::BuildOptions
ServeProto::Serialise<ServeProto::BuildOptions>::read(const store_dir_config_t& store, ReadConn conn) {
  BuildOptions options;
  options.max_silent_time = read_int(conn.from);
  options.buildTimeout = read_int(conn.from);
  if (GET_PROTOCOL_MINOR(conn.version) >= 2)
    options.maxLogSize = read_num<unsigned long>(conn.from);
  if (GET_PROTOCOL_MINOR(conn.version) >= 3) {
    options.nrRepeats = read_int(conn.from);
    options.enforceDeterminism = read_int(conn.from);
  }
  if (GET_PROTOCOL_MINOR(conn.version) >= 7) {
    options.keep_failed = (bool)read_int(conn.from);
  }
  return options;
}

void ServeProto::Serialise<ServeProto::BuildOptions>::write(
    const store_dir_config_t& store, WriteConn conn, const ServeProto::BuildOptions& options) {
  conn.to << options.max_silent_time << options.buildTimeout;
  if (GET_PROTOCOL_MINOR(conn.version) >= 2)
    conn.to << options.maxLogSize;
  if (GET_PROTOCOL_MINOR(conn.version) >= 3)
    conn.to << options.nrRepeats << options.enforceDeterminism;

  if (GET_PROTOCOL_MINOR(conn.version) >= 7) {
    conn.to << ((int)options.keep_failed);
  }
}

} // namespace nix
