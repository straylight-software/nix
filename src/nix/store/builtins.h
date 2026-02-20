#pragma once
///@file

#include "nix/store/config.h"
#include "nix/store/derivations.h"

#if NIX_WITH_AWS_AUTH
#  include "nix/store/aws-creds.h"
#endif

#include <nlohmann/json_fwd.hpp>

namespace nix {

struct StructuredAttrs;

struct BuiltinBuilderContext {
  const basic_derivation_t& drv;
  std::map<std::string, Path> outputs;
  std::string netrcData;
  std::string caFileData;
  Path tmp_dir_in_sandbox;

#if NIX_WITH_AWS_AUTH
  /**
   * Pre-resolved AWS credentials for S3 URLs in builtin:fetchurl.
   * When present, these should be used instead of creating new credential providers.
   */
  std::optional<AwsCredentials> awsCredentials;
#endif
};

using BuiltinBuilder = std::function<void(const BuiltinBuilderContext&)>;

struct RegisterBuiltinBuilder {
  typedef std::map<std::string, BuiltinBuilder> BuiltinBuilders;

  static BuiltinBuilders& builtinBuilders();

  RegisterBuiltinBuilder(const std::string& name, BuiltinBuilder&& fun) {
    builtinBuilders().insert_or_assign(name, std::move(fun));
  }
};

} // namespace nix
