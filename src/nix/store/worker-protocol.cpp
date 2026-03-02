#include "nix/store/worker-protocol.h"

#include <chrono>

#include <nlohmann/json.hpp>

#include "nix/store/build-result.h"
#include "nix/store/gc-store.h"
#include "nix/store/path-info.h"
#include "nix/store/path-with-outputs.h"
#include "nix/store/store-api.h"
#include "nix/store/worker-protocol-impl.h"
#include "nix/util/archive.h"
#include "nix/util/serialise.h"

namespace nix {

/* protocol-specific definitions */

BuildMode WorkerProto::Serialise<BuildMode>::read(const store_dir_config_t& store,
                                                  WorkerProto::ReadConn conn) {
  auto temp = read_num<uint8_t>(conn.from);
  switch (temp) {
    case 0:
      return bmNormal;
    case 1:
      return bmRepair;
    case 2:
      return bmCheck;
    default:
      throw Error("Invalid build mode");
  }
}

void WorkerProto::Serialise<BuildMode>::write(const store_dir_config_t& store,
                                              WorkerProto::WriteConn conn,
                                              const BuildMode& build_mode) {
  switch (build_mode) {
    case bmNormal:
      conn.to << uint8_t{0};
      break;
    case bmRepair:
      conn.to << uint8_t{1};
      break;
    case bmCheck:
      conn.to << uint8_t{2};
      break;
    default:
      throw Error("invalid build mode: %d", static_cast<int>(build_mode));
  };
}

GCAction WorkerProto::Serialise<GCAction>::read(const store_dir_config_t& store,
                                                WorkerProto::ReadConn conn) {
  auto temp = read_num<unsigned>(conn.from);
  using enum GCAction;
  switch (temp) {
    case 0:
      return gcReturnLive;
    case 1:
      return gcReturnDead;
    case 2:
      return gcDeleteDead;
    case 3:
      return gcDeleteSpecific;
    default:
      throw Error("Invalid GC action");
  }
}

void WorkerProto::Serialise<GCAction>::write(const store_dir_config_t& store,
                                             WorkerProto::WriteConn conn, const GCAction& action) {
  using enum GCAction;
  switch (action) {
    case gcReturnLive:
      conn.to << unsigned{0};
      break;
    case gcReturnDead:
      conn.to << unsigned{1};
      break;
    case gcDeleteDead:
      conn.to << unsigned{2};
      break;
    case gcDeleteSpecific:
      conn.to << unsigned{3};
      break;
    default:
      throw Error("invalid GC action: %d", static_cast<int>(action));
  }
}

std::optional<TrustedFlag>
WorkerProto::Serialise<std::optional<TrustedFlag>>::read(const store_dir_config_t& store,
                                                         WorkerProto::ReadConn conn) {
  auto temp = read_num<uint8_t>(conn.from);
  switch (temp) {
    case 0:
      return std::nullopt;
    case 1:
      return {Trusted};
    case 2:
      return {NotTrusted};
    default:
      throw Error("Invalid trusted status from remote");
  }
}

void WorkerProto::Serialise<std::optional<TrustedFlag>>::write(
    const store_dir_config_t& store, WorkerProto::WriteConn conn,
    const std::optional<TrustedFlag>& optTrusted) {
  if (!optTrusted) {
    conn.to << uint8_t{0};
  } else {
    switch (*optTrusted) {
      case Trusted:
        conn.to << uint8_t{1};
        break;
      case NotTrusted:
        conn.to << uint8_t{2};
        break;
      default:
        throw Error("invalid trusted flag: %d", static_cast<int>(*optTrusted));
    };
  }
}

std::optional<std::chrono::microseconds>
WorkerProto::Serialise<std::optional<std::chrono::microseconds>>::read(
    const store_dir_config_t& store, WorkerProto::ReadConn conn) {
  auto tag = read_num<uint8_t>(conn.from);
  switch (tag) {
    case 0:
      return std::nullopt;
    case 1:
      return std::optional<std::chrono::microseconds>{
          std::chrono::microseconds(read_num<int64_t>(conn.from))};
    default:
      throw Error("Invalid optional tag from remote");
  }
}

void WorkerProto::Serialise<std::optional<std::chrono::microseconds>>::write(
    const store_dir_config_t& store, WorkerProto::WriteConn conn,
    const std::optional<std::chrono::microseconds>& optDuration) {
  if (!optDuration.has_value()) {
    conn.to << uint8_t{0};
  } else {
    conn.to << uint8_t{1} << optDuration.value().count();
  }
}

derived_path_t WorkerProto::Serialise<derived_path_t>::read(const store_dir_config_t& store,
                                                            WorkerProto::ReadConn conn) {
  auto s = read_string(conn.from);
  if (GET_PROTOCOL_MINOR(conn.version) >= 30) {
    return derived_path_t::parseLegacy(store, s);
  } else {
    return parse_path_with_outputs(store, s).toDerivedPath();
  }
}

void WorkerProto::Serialise<derived_path_t>::write(const store_dir_config_t& store,
                                                   WorkerProto::WriteConn conn,
                                                   const derived_path_t& req) {
  if (GET_PROTOCOL_MINOR(conn.version) >= 30) {
    conn.to << req.to_string_legacy(store);
  } else {
    auto sOrDrvPath = StorePathWithOutputs::tryFromDerivedPath(req);
    std::visit(
        overloaded{
            [&](const StorePathWithOutputs& s) { conn.to << s.to_string(store); },
            [&](const store_path_t& drv_path) {
              throw Error("trying to request '%s', but daemon protocol %d.%d is too old (< 1.29) "
                          "to request a derivation file",
                          store.printStorePath(drv_path), GET_PROTOCOL_MAJOR(conn.version),
                          GET_PROTOCOL_MINOR(conn.version));
            },
            [&](std::monostate) {
              throw Error(
                  "wanted to build a derivation that is itself a build product, but protocols do "
                  "not support that. Try upgrading the Nix on the other end of this connection");
            },
        },
        sOrDrvPath);
  }
}

keyed_build_result_t
WorkerProto::Serialise<keyed_build_result_t>::read(const store_dir_config_t& store,
                                                   WorkerProto::ReadConn conn) {
  auto path = WorkerProto::Serialise<derived_path_t>::read(store, conn);
  auto br = WorkerProto::Serialise<build_result_t>::read(store, conn);
  return keyed_build_result_t{
      std::move(br),
      /* .path = */ std::move(path),
  };
}

void WorkerProto::Serialise<keyed_build_result_t>::write(const store_dir_config_t& store,
                                                         WorkerProto::WriteConn conn,
                                                         const keyed_build_result_t& res) {
  WorkerProto::write(store, conn, res.path);
  WorkerProto::write(store, conn, static_cast<const build_result_t&>(res));
}

build_result_t WorkerProto::Serialise<build_result_t>::read(const store_dir_config_t& store,
                                                            WorkerProto::ReadConn conn) {
  build_result_t res;
  build_result_t::Success success;
  build_result_t::Failure failure;

  auto rawStatus = read_int(conn.from);
  conn.from >> failure.errorMsg;

  if (GET_PROTOCOL_MINOR(conn.version) >= 29) {
    conn.from >> res.timesBuilt >> failure.isNonDeterministic >> res.start_time >> res.stopTime;
  }
  if (GET_PROTOCOL_MINOR(conn.version) >= 37) {
    res.cpu_user =
        WorkerProto::Serialise<std::optional<std::chrono::microseconds>>::read(store, conn);
    res.cpu_system =
        WorkerProto::Serialise<std::optional<std::chrono::microseconds>>::read(store, conn);
  }
  if (GET_PROTOCOL_MINOR(conn.version) >= 28) {
    auto built_outputs = WorkerProto::Serialise<DrvOutputs>::read(store, conn);
    for (auto&& [output, realisation] : built_outputs) {
      success.built_outputs.insert_or_assign(std::move(output.output_name), std::move(realisation));
    }
  }

  if (build_result_t::Success::statusIs(rawStatus)) {
    success.status = static_cast<build_result_t::Success::Status>(rawStatus);
    res.inner = std::move(success);
  } else {
    failure.status = static_cast<build_result_t::Failure::Status>(rawStatus);
    res.inner = std::move(failure);
  }

  return res;
}

void WorkerProto::Serialise<build_result_t>::write(const store_dir_config_t& store,
                                                   WorkerProto::WriteConn conn,
                                                   const build_result_t& res) {
  /* The protocol predates the use of sum types (std::variant) to
     separate the success or failure cases. As such, it transits some
     success- or failure-only fields in both cases. This helper
     function helps support this: in each case, we just pass the old
     default value for the fields that don't exist in that case. */
  auto common = [&](std::string_view errorMsg, bool isNonDeterministic, const auto& built_outputs) {
    conn.to << errorMsg;
    if (GET_PROTOCOL_MINOR(conn.version) >= 29) {
      conn.to << res.timesBuilt << isNonDeterministic << res.start_time << res.stopTime;
    }
    if (GET_PROTOCOL_MINOR(conn.version) >= 37) {
      WorkerProto::write(store, conn, res.cpu_user);
      WorkerProto::write(store, conn, res.cpu_system);
    }
    if (GET_PROTOCOL_MINOR(conn.version) >= 28) {
      DrvOutputs builtOutputsFullKey;
      for (auto& [output, realisation] : built_outputs) {
        builtOutputsFullKey.insert_or_assign(realisation.id, realisation);
      }
      WorkerProto::write(store, conn, builtOutputsFullKey);
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

valid_path_info_t WorkerProto::Serialise<valid_path_info_t>::read(const store_dir_config_t& store,
                                                                  ReadConn conn) {
  auto path = WorkerProto::Serialise<store_path_t>::read(store, conn);
  return valid_path_info_t{
      std::move(path),
      WorkerProto::Serialise<UnkeyedValidPathInfo>::read(store, conn),
  };
}

void WorkerProto::Serialise<valid_path_info_t>::write(const store_dir_config_t& store,
                                                      WriteConn conn,
                                                      const valid_path_info_t& path_info) {
  WorkerProto::write(store, conn, path_info.path);
  WorkerProto::write(store, conn, static_cast<const UnkeyedValidPathInfo&>(path_info));
}

UnkeyedValidPathInfo
WorkerProto::Serialise<UnkeyedValidPathInfo>::read(const store_dir_config_t& store, ReadConn conn) {
  auto deriver = WorkerProto::Serialise<std::optional<store_path_t>>::read(store, conn);
  auto nar_hash = Hash::parse_any(read_string(conn.from), hash_algorithm_t::SHA256);
  UnkeyedValidPathInfo info(store, nar_hash);
  info.deriver = std::move(deriver);
  info.references = WorkerProto::Serialise<store_path_set_t>::read(store, conn);
  conn.from >> info.registrationTime >> info.nar_size;
  if (GET_PROTOCOL_MINOR(conn.version) >= 16) {
    conn.from >> info.ultimate;
    info.sigs = read_strings<string_set_t>(conn.from);
    info.ca = content_address_t::parseOpt(read_string(conn.from));
  }
  return info;
}

void WorkerProto::Serialise<UnkeyedValidPathInfo>::write(const store_dir_config_t& store,
                                                         WriteConn conn,
                                                         const UnkeyedValidPathInfo& path_info) {
  WorkerProto::write(store, conn, path_info.deriver);
  conn.to << path_info.nar_hash.to_string(hash_format_t::base16, false);
  WorkerProto::write(store, conn, path_info.references);
  conn.to << path_info.registrationTime << path_info.nar_size;
  if (GET_PROTOCOL_MINOR(conn.version) >= 16) {
    conn.to << path_info.ultimate << path_info.sigs << render_content_address(path_info.ca);
  }
}

WorkerProto::ClientHandshakeInfo
WorkerProto::Serialise<WorkerProto::ClientHandshakeInfo>::read(const store_dir_config_t& store,
                                                               ReadConn conn) {
  WorkerProto::ClientHandshakeInfo res;

  if (GET_PROTOCOL_MINOR(conn.version) >= 33) {
    res.daemonNixVersion = read_string(conn.from);
  }

  if (GET_PROTOCOL_MINOR(conn.version) >= 35) {
    res.remoteTrustsUs = WorkerProto::Serialise<std::optional<TrustedFlag>>::read(store, conn);
  } else {
    // We don't know the answer; protocol to old.
    res.remoteTrustsUs = std::nullopt;
  }

  return res;
}

void WorkerProto::Serialise<WorkerProto::ClientHandshakeInfo>::write(
    const store_dir_config_t& store, WriteConn conn, const WorkerProto::ClientHandshakeInfo& info) {
  if (GET_PROTOCOL_MINOR(conn.version) >= 33) {
    assert(info.daemonNixVersion);
    conn.to << *info.daemonNixVersion;
  }

  if (GET_PROTOCOL_MINOR(conn.version) >= 35) {
    WorkerProto::write(store, conn, info.remoteTrustsUs);
  }
}

} // namespace nix
