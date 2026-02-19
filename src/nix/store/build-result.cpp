#include "nix/store/build-result.h"

#include <array>

#include "nix/util/json-utils.h"

namespace nix {

bool BuildResult::operator==(const BuildResult&) const noexcept = default;
std::strong_ordering BuildResult::operator<=>(const BuildResult&) const noexcept = default;

bool BuildResult::Success::operator==(const BuildResult::Success&) const noexcept = default;
std::strong_ordering
BuildResult::Success::operator<=>(const BuildResult::Success&) const noexcept = default;

bool BuildResult::Failure::operator==(const BuildResult::Failure&) const noexcept = default;
std::strong_ordering
BuildResult::Failure::operator<=>(const BuildResult::Failure&) const noexcept = default;

static constexpr std::array<std::pair<BuildResult::Success::Status, std::string_view>, 4>
    successStatusStrings{{
#define ENUM_ENTRY(e) {BuildResult::Success::e, #e}
        ENUM_ENTRY(Built),
        ENUM_ENTRY(Substituted),
        ENUM_ENTRY(AlreadyValid),
        ENUM_ENTRY(ResolvesToAlreadyValid),
#undef ENUM_ENTRY
    }};

std::string_view BuildResult::Success::status_to_string(BuildResult::Success::Status status) {
  for (const auto& [enumVal, str] : successStatusStrings) {
    if (enumVal == status)
      return str;
  }
  throw Error("unknown success status: %d", static_cast<int>(status));
}

static BuildResult::Success::Status successStatusFromString(std::string_view str) {
  for (const auto& [enumVal, enumStr] : successStatusStrings) {
    if (enumStr == str)
      return enumVal;
  }
  throw Error("unknown built result success status '%s'", str);
}

static constexpr std::array<std::pair<BuildResult::Failure::Status, std::string_view>, 13>
    failureStatusStrings{{
#define ENUM_ENTRY(e) {BuildResult::Failure::e, #e}
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

std::string_view BuildResult::Failure::status_to_string(BuildResult::Failure::Status status) {
  for (const auto& [enumVal, str] : failureStatusStrings) {
    if (enumVal == status)
      return str;
  }
  throw Error("unknown failure status: %d", static_cast<int>(status));
}

static BuildResult::Failure::Status failureStatusFromString(std::string_view str) {
  for (const auto& [enumVal, enumStr] : failureStatusStrings) {
    if (enumStr == str)
      return enumVal;
  }
  throw Error("unknown built result failure status '%s'", str);
}

} // namespace nix

namespace nlohmann {

using namespace nix;

void adl_serializer<BuildResult>::to_json(json& res, const BuildResult& br) {
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
  std::visit(overloaded{
                 [&](const BuildResult::Success& success) {
                   res["success"] = true;
                   res["status"] = BuildResult::Success::status_to_string(success.status);
                   res["builtOutputs"] = success.built_outputs;
                 },
                 [&](const BuildResult::Failure& failure) {
                   res["success"] = false;
                   res["status"] = BuildResult::Failure::status_to_string(failure.status);
                   res["errorMsg"] = failure.errorMsg;
                   res["isNonDeterministic"] = failure.isNonDeterministic;
                 },
             },
             br.inner);
}

BuildResult adl_serializer<BuildResult>::from_json(const json& _json) {
  auto& json = get_object(_json);

  BuildResult br;

  // Common fields
  br.timesBuilt = get_unsigned(value_at(json, "timesBuilt"));
  br.start_time = get_unsigned(value_at(json, "startTime"));
  br.stopTime = get_unsigned(value_at(json, "stopTime"));

  if (auto cpu_user = optional_value_at(json, "cpuUser")) {
    br.cpu_user = std::chrono::microseconds(get_unsigned(*cpu_user));
  }
  if (auto cpu_system = optional_value_at(json, "cpuSystem")) {
    br.cpu_system = std::chrono::microseconds(get_unsigned(*cpu_system));
  }

  // Determine success or failure based on success field
  bool success = get_boolean(value_at(json, "success"));
  std::string statusStr = get_string(value_at(json, "status"));

  if (success) {
    BuildResult::Success s;
    s.status = successStatusFromString(statusStr);
    s.built_outputs = value_at(json, "builtOutputs");
    br.inner = std::move(s);
  } else {
    BuildResult::Failure f;
    f.status = failureStatusFromString(statusStr);
    f.errorMsg = get_string(value_at(json, "errorMsg"));
    f.isNonDeterministic = get_boolean(value_at(json, "isNonDeterministic"));
    br.inner = std::move(f);
  }

  return br;
}

KeyedBuildResult adl_serializer<KeyedBuildResult>::from_json(const json& json0) {
  auto json = get_object(json0);

  return KeyedBuildResult{
      adl_serializer<BuildResult>::from_json(json0),
      value_at(json, "path"),
  };
}

void adl_serializer<KeyedBuildResult>::to_json(json& json, const KeyedBuildResult& kbr) {
  adl_serializer<BuildResult>::to_json(json, kbr);
  json["path"] = kbr.path;
}

} // namespace nlohmann
