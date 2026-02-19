#include "nix/store/common-protocol.h"

#include <nlohmann/json.hpp>

#include "nix/store/build-result.h"
#include "nix/store/common-protocol-impl.h"
#include "nix/store/derivations.h"
#include "nix/store/path-with-outputs.h"
#include "nix/store/store-api.h"
#include "nix/util/archive.h"
#include "nix/util/serialise.h"

namespace nix {

/* protocol-agnostic definitions */

std::string CommonProto::Serialise<std::string>::read(const StoreDirConfig& store,
                                                      CommonProto::ReadConn conn) {
  return read_string(conn.from);
}

void CommonProto::Serialise<std::string>::write(const StoreDirConfig& store,
                                                CommonProto::WriteConn conn,
                                                const std::string& str) {
  conn.to << str;
}

StorePath CommonProto::Serialise<StorePath>::read(const StoreDirConfig& store,
                                                  CommonProto::ReadConn conn) {
  return conn.shortStorePaths ? StorePath(read_string(conn.from))
                              : store.parseStorePath(read_string(conn.from));
}

void CommonProto::Serialise<StorePath>::write(const StoreDirConfig& store,
                                              CommonProto::WriteConn conn,
                                              const StorePath& store_path) {
  conn.to << (conn.shortStorePaths ? store_path.to_string() : store.printStorePath(store_path));
}

ContentAddress CommonProto::Serialise<ContentAddress>::read(const StoreDirConfig& store,
                                                            CommonProto::ReadConn conn) {
  return ContentAddress::parse(read_string(conn.from));
}

void CommonProto::Serialise<ContentAddress>::write(const StoreDirConfig& store,
                                                   CommonProto::WriteConn conn,
                                                   const ContentAddress& ca) {
  conn.to << render_content_address(ca);
}

Realisation CommonProto::Serialise<Realisation>::read(const StoreDirConfig& store,
                                                      CommonProto::ReadConn conn) {
  std::string rawInput = read_string(conn.from);
  try {
    return nlohmann::json::parse(rawInput);
  } catch (Error& e) {
    e.add_trace({}, "while parsing a realisation object in the remote protocol");
    throw;
  }
}

void CommonProto::Serialise<Realisation>::write(const StoreDirConfig& store,
                                                CommonProto::WriteConn conn,
                                                const Realisation& realisation) {
  conn.to << static_cast<nlohmann::json>(realisation).dump();
}

DrvOutput CommonProto::Serialise<DrvOutput>::read(const StoreDirConfig& store,
                                                  CommonProto::ReadConn conn) {
  return DrvOutput::parse(read_string(conn.from));
}

void CommonProto::Serialise<DrvOutput>::write(const StoreDirConfig& store,
                                              CommonProto::WriteConn conn,
                                              const DrvOutput& drvOutput) {
  conn.to << drvOutput.to_string();
}

std::optional<StorePath>
CommonProto::Serialise<std::optional<StorePath>>::read(const StoreDirConfig& store,
                                                       CommonProto::ReadConn conn) {
  auto s = read_string(conn.from);
  return s == ""                ? std::optional<StorePath>{}
         : conn.shortStorePaths ? StorePath(s)
                                : store.parseStorePath(s);
}

void CommonProto::Serialise<std::optional<StorePath>>::write(
    const StoreDirConfig& store, CommonProto::WriteConn conn,
    const std::optional<StorePath>& storePathOpt) {
  conn.to << (storePathOpt ? (conn.shortStorePaths ? storePathOpt->to_string()
                                                   : store.printStorePath(*storePathOpt))
                           : "");
}

std::optional<ContentAddress>
CommonProto::Serialise<std::optional<ContentAddress>>::read(const StoreDirConfig& store,
                                                            CommonProto::ReadConn conn) {
  return ContentAddress::parseOpt(read_string(conn.from));
}

void CommonProto::Serialise<std::optional<ContentAddress>>::write(
    const StoreDirConfig& store, CommonProto::WriteConn conn,
    const std::optional<ContentAddress>& caOpt) {
  conn.to << (caOpt ? render_content_address(*caOpt) : "");
}

} // namespace nix
