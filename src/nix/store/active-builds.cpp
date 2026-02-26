#include "nix/store/active-builds.h"

#include <nlohmann/json.hpp>

#include "nix/util/json-utils.h"

#ifndef _WIN32
#  include <pwd.h>
#endif

namespace nix {

UserInfo UserInfo::fromUid(uid_t uid) {
  UserInfo info;
  info.uid = uid;

#ifndef _WIN32
  // Look up the user name for the UID (thread-safe)
  struct passwd pwd;
  struct passwd* result;
  std::vector<char> buf(16384);
  if (getpwuid_r(uid, &pwd, buf.data(), buf.size(), &result) == 0 && result)
    info.name = result->pw_name;
#endif

  return info;
}

} // namespace nix

namespace nlohmann {

nix::UserInfo adl_serializer<nix::UserInfo>::from_json(const json& j) {
  return nix::UserInfo{
      .uid = j.at("uid").get<uid_t>(),
      .name = j.contains("name") && !j.at("name").is_null()
                  ? std::optional<std::string>(j.at("name").get<std::string>())
                  : std::nullopt,
  };
}

void adl_serializer<nix::UserInfo>::to_json(json& j, const nix::UserInfo& info) {
  j = nlohmann::json{
      {"uid", info.uid},
      {"name", info.name},
  };
}

// Durations are serialized as floats representing seconds.
static std::optional<std::chrono::microseconds> parse_duration(const json& j, const char* key) {
  if (j.contains(key) && !j.at(key).is_null())
    return std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::duration<float, std::chrono::seconds::period>(j.at(key).get<double>()));
  else
    return std::nullopt;
}

static nlohmann::json print_duration(const std::optional<std::chrono::microseconds>& duration) {
  return duration ? nlohmann::json(
                        std::chrono::duration_cast<
                            std::chrono::duration<float, std::chrono::seconds::period>>(*duration)
                            .count())
                  : nullptr;
}

nix::ActiveBuildInfo::ProcessInfo
adl_serializer<nix::ActiveBuildInfo::ProcessInfo>::from_json(const json& j) {
  return nix::ActiveBuildInfo::ProcessInfo{
      .pid = j.at("pid").get<::pid_t>(),
      .parent_pid = j.at("parentPid").get<::pid_t>(),
      .user = j.at("user").get<nix::UserInfo>(),
      .argv = j.at("argv").get<std::vector<std::string>>(),
      .utime = parse_duration(j, "utime"),
      .stime = parse_duration(j, "stime"),
      .cutime = parse_duration(j, "cutime"),
      .cstime = parse_duration(j, "cstime"),
  };
}

void adl_serializer<nix::ActiveBuildInfo::ProcessInfo>::to_json(
    json& j, const nix::ActiveBuildInfo::ProcessInfo& process) {
  j = nlohmann::json{
      {"pid", process.pid},
      {"parentPid", process.parent_pid},
      {"user", process.user},
      {"argv", process.argv},
      {"utime", print_duration(process.utime)},
      {"stime", print_duration(process.stime)},
      {"cutime", print_duration(process.cutime)},
      {"cstime", print_duration(process.cstime)},
  };
}

nix::ActiveBuild adl_serializer<nix::ActiveBuild>::from_json(const json& j) {
  auto type = j.at("type").get<std::string>();
  if (type != "build")
    throw nix::Error("invalid active build JSON: expected type 'build' but got '%s'", type);
  return nix::ActiveBuild{
      .nix_pid = j.at("nixPid").get<::pid_t>(),
      .client_pid = j.at("clientPid").get<std::optional<::pid_t>>(),
      .clientUid = j.at("clientUid").get<std::optional<uid_t>>(),
      .main_pid = j.at("mainPid").get<::pid_t>(),
      .mainUser = j.at("mainUser").get<nix::UserInfo>(),
      .cgroup = j.at("cgroup").get<std::optional<nix::Path>>(),
      .start_time = (time_t)j.at("startTime").get<double>(),
      .derivation = nix::store_path_t{nix::get_string(j.at("derivation"))},
  };
}

void adl_serializer<nix::ActiveBuild>::to_json(json& j, const nix::ActiveBuild& build) {
  j = nlohmann::json{
      {"type", "build"},
      {"nixPid", build.nix_pid},
      {"clientPid", build.client_pid},
      {"clientUid", build.clientUid},
      {"mainPid", build.main_pid},
      {"mainUser", build.mainUser},
      {"cgroup", build.cgroup},
      {"startTime", (double)build.start_time},
      {"derivation", build.derivation.to_string()},
  };
}

nix::ActiveBuildInfo adl_serializer<nix::ActiveBuildInfo>::from_json(const json& j) {
  nix::ActiveBuildInfo info(adl_serializer<nix::ActiveBuild>::from_json(j));
  info.processes = j.at("processes").get<std::vector<nix::ActiveBuildInfo::ProcessInfo>>();
  info.utime = parse_duration(j, "utime");
  info.stime = parse_duration(j, "stime");
  return info;
}

void adl_serializer<nix::ActiveBuildInfo>::to_json(json& j, const nix::ActiveBuildInfo& build) {
  adl_serializer<nix::ActiveBuild>::to_json(j, build);
  j["processes"] = build.processes;
  j["utime"] = print_duration(build.utime);
  j["stime"] = print_duration(build.stime);
}

} // namespace nlohmann
