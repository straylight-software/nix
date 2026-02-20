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

std::string CommonProto::Serialise<std::string>::read(const store_dir_config_t& store,
                                                      CommonProto::ReadConn conn) {
  return read_string(conn.from);
}

void CommonProto::Serialise<std::string>::write(const store_dir_config_t& store,
                                                CommonProto::WriteConn conn,
                                                const std::string& str) {
  conn.to << str;
}

store_path_t CommonProto::Serialise<store_path_t>::read(const store_dir_config_t& store,
                                                        CommonProto::ReadConn conn) {
  return conn.shortStorePaths ? store_path_t(read_string(conn.from))
                              : store.parseStorePath(read_string(conn.from));
}

void CommonProto::Serialise<store_path_t>::write(const store_dir_config_t& store,
                                                 CommonProto::WriteConn conn,
                                                 const store_path_t& store_path) {
  conn.to << (conn.shortStorePaths ? store_path.to_string() : store.printStorePath(store_path));
}

content_address_t CommonProto::Serialise<content_address_t>::read(const store_dir_config_t& store,
                                                                  CommonProto::ReadConn conn) {
  return content_address_t::parse(read_string(conn.from));
}

void CommonProto::Serialise<content_address_t>::write(const store_dir_config_t& store,
                                                      CommonProto::WriteConn conn,
                                                      const content_address_t& ca) {
  conn.to << render_content_address(ca);
}

realisation_t CommonProto::Serialise<realisation_t>::read(const store_dir_config_t& store,
                                                          CommonProto::ReadConn conn) {
  std::string rawInput = read_string(conn.from);
  try {
    return nlohmann::json::parse(rawInput);
  } catch (Error& e) {
    e.add_trace({}, "while parsing a realisation object in the remote protocol");
    throw;
  }
}

void CommonProto::Serialise<realisation_t>::write(const store_dir_config_t& store,
                                                  CommonProto::WriteConn conn,
                                                  const realisation_t& realisation) {
  conn.to << static_cast<nlohmann::json>(realisation).dump();
}

DrvOutput CommonProto::Serialise<DrvOutput>::read(const store_dir_config_t& store,
                                                  CommonProto::ReadConn conn) {
  return DrvOutput::parse(read_string(conn.from));
}

void CommonProto::Serialise<DrvOutput>::write(const store_dir_config_t& store,
                                              CommonProto::WriteConn conn,
                                              const DrvOutput& drvOutput) {
  conn.to << drvOutput.to_string();
}

std::optional<store_path_t>
CommonProto::Serialise<std::optional<store_path_t>>::read(const store_dir_config_t& store,
                                                          CommonProto::ReadConn conn) {
  auto s = read_string(conn.from);
  return s == ""                ? std::optional<store_path_t>{}
         : conn.shortStorePaths ? store_path_t(s)
                                : store.parseStorePath(s);
}

void CommonProto::Serialise<std::optional<store_path_t>>::write(
    const store_dir_config_t& store, CommonProto::WriteConn conn,
    const std::optional<store_path_t>& storePathOpt) {
  conn.to << (storePathOpt ? (conn.shortStorePaths ? storePathOpt->to_string()
                                                   : store.printStorePath(*storePathOpt))
                           : "");
}

std::optional<content_address_t>
CommonProto::Serialise<std::optional<content_address_t>>::read(const store_dir_config_t& store,
                                                               CommonProto::ReadConn conn) {
  return content_address_t::parseOpt(read_string(conn.from));
}

void CommonProto::Serialise<std::optional<content_address_t>>::write(
    const store_dir_config_t& store, CommonProto::WriteConn conn,
    const std::optional<content_address_t>& caOpt) {
  conn.to << (caOpt ? render_content_address(*caOpt) : "");
}

} // namespace nix
