#pragma once
///@file

#include <memory>

#include "nix/util/types.h"

namespace nix {

struct regex_t;

struct DrvName {
  std::string fullName;
  std::string name;
  std::string version;
  unsigned int hits;

  DrvName();
  DrvName(std::string_view s);
  ~DrvName();

  bool matches(const DrvName& n);

private:
  std::unique_ptr<regex_t> regex;
};

typedef std::list<DrvName> DrvNames;

std::string_view next_component(std::string_view::const_iterator& p,
                               const std::string_view::const_iterator end);
std::strong_ordering compare_versions(const std::string_view v1, const std::string_view v2);
DrvNames drv_names_from_args(const strings_t& op_args);

} // namespace nix
