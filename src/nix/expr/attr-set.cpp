#include "nix/expr/attr-set.h"

#include <algorithm>

#include "nix/expr/eval-inline.h"

namespace nix {

bindings_t bindings_t::emptyBindings;

/* Allocate a new array of attributes for an attribute set with a specific
   capacity. The space is implicitly reserved after the bindings_t
   structure. */
bindings_t* EvalMemory::allocBindings(size_t capacity) {
  if (capacity == 0)
    return &bindings_t::emptyBindings;
  if (capacity > std::numeric_limits<bindings_t::size_type>::max())
    throw Error("attribute set of size %d is too big", capacity);
  stats.nrAttrsets++;
  stats.nrAttrsInAttrsets += capacity;
  return new (allocBytes(sizeof(bindings_t) + sizeof(attr_t) * capacity)) bindings_t();
}

value_t& BindingsBuilder::alloc(symbol_t name, pos_idx_t pos) {
  auto value = mem.get().allocValue();
  bindings->push_back(attr_t(name, value, pos));
  return *value;
}

value_t& BindingsBuilder::alloc(std::string_view name, pos_idx_t pos) {
  return alloc(symbols.get().create(name), pos);
}

void bindings_t::sort() {
  std::sort(attrs, attrs + numAttrs);
}

value_t& value_t::mkAttrs(BindingsBuilder& bindings) {
  mkAttrs(bindings.finish());
  return *this;
}

} // namespace nix
