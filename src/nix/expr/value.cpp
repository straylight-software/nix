#include "nix/expr/value.h"

namespace nix {

value_t value_t::vEmptyList = []() {
  value_t res;
  res.setStorage(List{.size = 0, .elems = nullptr});
  return res;
}();

value_t value_t::vNull = []() {
  value_t res;
  res.mkNull();
  return res;
}();

value_t value_t::vTrue = []() {
  value_t res;
  res.mkBool(true);
  return res;
}();

value_t value_t::vFalse = []() {
  value_t res;
  res.mkBool(false);
  return res;
}();

} // namespace nix
