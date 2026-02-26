#include "nix/store/build-result.h"

#include <array>

#include "nix/util/json-utils.h"

namespace nix {

bool build_result_t::operator==(const build_result_t&) const noexcept = default;
std::strong_ordering build_result_t::operator<=>(const build_result_t&) const noexcept = default;

bool build_result_t::Success::operator==(const build_result_t::Success&) const noexcept = default;
std::strong_ordering
build_result_t::Success::operator<=>(const build_result_t::Success&) const noexcept = default;

bool build_result_t::Failure::operator==(const build_result_t::Failure&) const noexcept = default;
std::strong_ordering
build_result_t::Failure::operator<=>(const build_result_t::Failure&) const noexcept = default;

static constexpr std::array<std::pair<build_result_t::Success::Status, std::string_view>, 4>
    successStatusStrings{{
#define ENUM_ENTRY(e) {build_result_t::Success::e, #e}
        ENUM_ENTRY(Built),
        ENUM_ENTRY(Substituted),
        ENUM_ENTRY(AlreadyValid),
        ENUM_ENTRY(ResolvesToAlreadyValid),
#undef ENUM_ENTRY
    }};

std::string_view build_result_t::Success::status_to_string(build_result_t::Success::Status status) {
  for (const auto& [enumVal, str] : successStatusStrings) {
    if (enumVal == status)
      return str;
  }
  throw Error("unknown success status: %d", static_cast<int>(status));
}

static build_result_t::Success::Status successStatusFromString(std::string_view str) {
  for (const auto& [enumVal, enumStr] : successStatusStrings) {
    if (enumStr == str)
      return enumVal;
  }
  throw Error("unknown built result success status '%s'", str);
}

static constexpr std::array<std::pair<build_result_t::Failure::Status, std::string_view>, 13>
    failureStatusStrings{{
#define ENUM_ENTRY(e) {build_result_t::Failure::e, #e}
        ENUM_ENTRY(PermanentFailure),
        ENUM_ENTRY(InputRejected),
        ENUM_ENTRY(OutputRejected),
        ENUM_ENTRY(TransientFailure),
        ENUM_ENTRY(CachedFailure),
        ENUM_ENTRY(TimedOut),
        ENUM_ENTRY(MiscFailure),
        ENUM_ENTRY(DependencyFailed),
        ENUM_ENTRY(LogLimitExceeded),
        ENUM_ENTRY(not_deterministic_t),
        ENUM_ENTRY(NoSubstituters),
        ENUM_ENTRY(HashMismatch),
        ENUM_ENTRY(Cancelled),
#undef ENUM_ENTRY
    }};

std::string_view build_result_t::Failure::status_to_string(build_result_t::Failure::Status status) {
  for (const auto& [enumVal, str] : failureStatusStrings) {
    if (enumVal == status)
      return str;
  }
  throw Error("unknown failure status: %d", static_cast<int>(status));
}

static build_result_t::Failure::Status failureStatusFromString(std::string_view str) {
  for (const auto& [enumVal, enumStr] : failureStatusStrings) {
    if (enumStr == str)
      return enumVal;
  }
  throw Error("unknown built result failure status '%s'", str);
}

} // namespace nix

namespace nlohmann {

void adl_serializer<nix::build_result_t>::to_json(json& res, const nix::build_result_t& br) {
  res = json::object();

  // Common fields
  res["timesBuilt"] = br.timesBuilt;
  res["startTime"] = br.start_time;
  res["stopTime"] = br.stopTime;

  if (br.cpu_user.has_value()) {
    res["cpuUser"] = br.cpu_user->count();
  }
  if (br.cpu_system.has_value()) {
    res["cpuSystem"] = br.cpu_system->count();
  }

  // Handle success or failure variant
  std::visit(nix::overloaded{
                 [&](const nix::build_result_t::Success& success) {
                   res["success"] = true;
                   res["status"] = nix::build_result_t::Success::status_to_string(success.status);
                   res["builtOutputs"] = success.built_outputs;
                 },
                 [&](const nix::build_result_t::Failure& failure) {
                   res["success"] = false;
                   res["status"] = nix::build_result_t::Failure::status_to_string(failure.status);
                   res["errorMsg"] = failure.errorMsg;
                   res["isNonDeterministic"] = failure.isNonDeterministic;
                 },
             },
             br.inner);
}

nix::build_result_t adl_serializer<nix::build_result_t>::from_json(const json& _json) {
  auto& json = nix::get_object(_json);

  nix::build_result_t br;

  // Common fields
  br.timesBuilt = nix::get_unsigned(nix::value_at(json, "timesBuilt"));
  br.start_time = nix::get_unsigned(nix::value_at(json, "startTime"));
  br.stopTime = nix::get_unsigned(nix::value_at(json, "stopTime"));

  if (auto cpu_user = nix::optional_value_at(json, "cpuUser")) {
    br.cpu_user = std::chrono::microseconds(nix::get_unsigned(*cpu_user));
  }
  if (auto cpu_system = nix::optional_value_at(json, "cpuSystem")) {
    br.cpu_system = std::chrono::microseconds(nix::get_unsigned(*cpu_system));
  }

  // Determine success or failure based on success field
  bool success = nix::get_boolean(nix::value_at(json, "success"));
  std::string statusStr = nix::get_string(nix::value_at(json, "status"));

  if (success) {
    nix::build_result_t::Success s;
    s.status = successStatusFromString(statusStr);
    s.built_outputs = nix::value_at(json, "builtOutputs");
    br.inner = std::move(s);
  } else {
    nix::build_result_t::Failure f;
    f.status = failureStatusFromString(statusStr);
    f.errorMsg = nix::get_string(nix::value_at(json, "errorMsg"));
    f.isNonDeterministic = nix::get_boolean(nix::value_at(json, "isNonDeterministic"));
    br.inner = std::move(f);
  }

  return br;
}

nix::keyed_build_result_t adl_serializer<nix::keyed_build_result_t>::from_json(const json& json0) {
  auto json = nix::get_object(json0);

  return nix::keyed_build_result_t{
      adl_serializer<nix::build_result_t>::from_json(json0),
      nix::value_at(json, "path"),
  };
}

void adl_serializer<nix::keyed_build_result_t>::to_json(json& json,
                                                        const nix::keyed_build_result_t& kbr) {
  adl_serializer<nix::build_result_t>::to_json(json, kbr);
  json["path"] = kbr.path;
}

} // namespace nlohmann
