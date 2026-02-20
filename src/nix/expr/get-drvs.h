#pragma once
///@file

#include <map>
#include <string>

#include "nix/expr/eval.h"
#include "nix/store/path.h"

namespace nix {

/**
 * A "parsed" package attribute set.
 */
struct PackageInfo {
public:
  typedef std::map<std::string, std::optional<store_path_t>, std::less<>> Outputs;

private:
  eval_state_t* state;

  mutable std::string name;
  mutable std::string system;
  mutable std::optional<std::optional<store_path_t>> drv_path;
  mutable std::optional<store_path_t> out_path;
  mutable std::string output_name;
  Outputs outputs;

  /**
   * Set if we get an AssertionError
   */
  bool failed = false;

  const bindings_t *attrs = nullptr, *meta = nullptr;

  const bindings_t* getMeta();

  bool checkMeta(value_t& v);

public:
  /**
   * path towards the derivation
   */
  std::string attr_path;

  PackageInfo(eval_state_t& state) : state(&state) {};
  PackageInfo(eval_state_t& state, std::string attr_path, const bindings_t* attrs);
  PackageInfo(eval_state_t& state, ref<store_t> store, const std::string& drvPathWithOutputs);

  std::string queryName() const;
  std::string querySystem() const;
  std::optional<store_path_t> queryDrvPath() const;
  store_path_t requireDrvPath() const;
  store_path_t queryOutPath() const;
  std::string queryOutputName() const;
  /**
   * Return the unordered map of output names to (optional) output paths.
   * The "outputs to install" are determined by `meta.outputsToInstall`.
   */
  Outputs queryOutputs(bool withPaths = true, bool onlyOutputsToInstall = false);

  string_set_t queryMetaNames();
  value_t* queryMeta(const std::string& name);
  std::string queryMetaString(const std::string& name);
  NixInt queryMetaInt(const std::string& name, NixInt def);
  NixFloat queryMetaFloat(const std::string& name, NixFloat def);
  bool queryMetaBool(const std::string& name, bool def);
  void setMeta(const std::string& name, value_t* v);

  /*
  MetaInfo queryMetaInfo(eval_state_t & state) const;
  MetaValue queryMetaInfo(eval_state_t & state, const string & name) const;
  */

  void setName(const std::string& s) { name = s; }

  void setDrvPath(store_path_t path) { drv_path = {{std::move(path)}}; }

  void setOutPath(store_path_t path) { out_path = {{std::move(path)}}; }

  void set_failed() { failed = true; };

  bool hasFailed() { return failed; };
};

using PackageInfos = std::list<PackageInfo, traceable_allocator<PackageInfo>>;

/**
 * If value `v` denotes a derivation, return a PackageInfo object
 * describing it. Otherwise return nothing.
 */
std::optional<PackageInfo> get_derivation(eval_state_t& state, value_t& v, bool ignore_assertion_failures);

void get_derivations(eval_state_t& state, value_t& v, const std::string& path_prefix, bindings_t& auto_args,
                    PackageInfos& drvs, bool ignore_assertion_failures);

} // namespace nix
