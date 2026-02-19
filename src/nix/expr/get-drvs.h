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
  typedef std::map<std::string, std::optional<StorePath>, std::less<>> Outputs;

private:
  EvalState* state;

  mutable std::string name;
  mutable std::string system;
  mutable std::optional<std::optional<StorePath>> drv_path;
  mutable std::optional<StorePath> out_path;
  mutable std::string output_name;
  Outputs outputs;

  /**
   * Set if we get an AssertionError
   */
  bool failed = false;

  const Bindings *attrs = nullptr, *meta = nullptr;

  const Bindings* getMeta();

  bool checkMeta(Value& v);

public:
  /**
   * path towards the derivation
   */
  std::string attr_path;

  PackageInfo(EvalState& state) : state(&state) {};
  PackageInfo(EvalState& state, std::string attr_path, const Bindings* attrs);
  PackageInfo(EvalState& state, ref<Store> store, const std::string& drvPathWithOutputs);

  std::string queryName() const;
  std::string querySystem() const;
  std::optional<StorePath> queryDrvPath() const;
  StorePath requireDrvPath() const;
  StorePath queryOutPath() const;
  std::string queryOutputName() const;
  /**
   * Return the unordered map of output names to (optional) output paths.
   * The "outputs to install" are determined by `meta.outputsToInstall`.
   */
  Outputs queryOutputs(bool withPaths = true, bool onlyOutputsToInstall = false);

  string_set_t queryMetaNames();
  Value* queryMeta(const std::string& name);
  std::string queryMetaString(const std::string& name);
  NixInt queryMetaInt(const std::string& name, NixInt def);
  NixFloat queryMetaFloat(const std::string& name, NixFloat def);
  bool queryMetaBool(const std::string& name, bool def);
  void setMeta(const std::string& name, Value* v);

  /*
  MetaInfo queryMetaInfo(EvalState & state) const;
  MetaValue queryMetaInfo(EvalState & state, const string & name) const;
  */

  void setName(const std::string& s) { name = s; }

  void setDrvPath(StorePath path) { drv_path = {{std::move(path)}}; }

  void setOutPath(StorePath path) { out_path = {{std::move(path)}}; }

  void set_failed() { failed = true; };

  bool hasFailed() { return failed; };
};

typedef std::list<PackageInfo, traceable_allocator<PackageInfo>> PackageInfos;

/**
 * If value `v` denotes a derivation, return a PackageInfo object
 * describing it. Otherwise return nothing.
 */
std::optional<PackageInfo> get_derivation(EvalState& state, Value& v, bool ignore_assertion_failures);

void get_derivations(EvalState& state, Value& v, const std::string& path_prefix, Bindings& auto_args,
                    PackageInfos& drvs, bool ignore_assertion_failures);

} // namespace nix
