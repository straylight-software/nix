#include "nix/flake/flake-primops.h"

#include <functional>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <variant>
#include <vector>

#include <stdint.h>

#include "nix/expr/attr-set.h"
#include "nix/expr/eval-error.h"
#include "nix/expr/eval-inline.h"
#include "nix/expr/eval-settings.h"
#include "nix/expr/eval.h"
#include "nix/expr/symbol-table.h"
#include "nix/expr/value.h"
#include "nix/fetchers/attrs.h"
#include "nix/fetchers/fetchers.h"
#include "nix/flake/flake.h"
#include "nix/flake/flakeref.h"
#include "nix/flake/settings.h"
#include "nix/util/configuration.h"
#include "nix/util/error.h"
#include "nix/util/experimental-features.h"
#include "nix/util/pos-idx.h"
#include "nix/util/pos-table.h"
#include "nix/util/source-path.h"
#include "nix/util/types.h"
#include "nix/util/util.h"

namespace nix::flake::primops {

PrimOp get_flake(const settings_t& settings) {
  auto prim_get_flake = [&settings](eval_state_t& state, const pos_idx_t pos, value_t** args,
                                    value_t& v) {
    std::string flake_ref_s(state.forceStringNoCtx(
        *args[0], pos, "while evaluating the argument passed to builtins.getFlake"));
    auto flake_ref = nix::parse_flake_ref(state.fetch_settings, flake_ref_s, {}, true);
    if (state.settings.pureEval && !flake_ref.input.isLocked(state.fetch_settings)) {
      throw Error("cannot call 'getFlake' on unlocked flake reference '%s', at %s (use --impure to "
                  "override)",
                  flake_ref_s, state.positions[pos]);
    }

    call_flake(state,
               lock_flake(settings, state, flake_ref,
                          LockFlags{
                              .updateLockFile = false,
                              .writeLockFile = false,
                              .use_registries = !state.settings.pureEval && settings.use_registries,
                              .allowUnlocked = !state.settings.pureEval,
                          }),
               v);
  };

  return PrimOp{
      .name = "__getFlake",
      .args = {"args"},
      .doc = R"(
          Fetch a flake from a flake reference, and return its output attributes and some metadata. For example:

          ```nix
          (builtins.get_flake "nix/55bc52401966fbffa525c574c14f67b00bc4fb3a").packages.x86_64-linux.nix
          ```

          Unless impure evaluation is allowed (`--impure`), the flake reference
          must be "locked", e.g. contain a git revision or content hash. An
          example of an unlocked usage is:

          ```nix
          (builtins.get_flake "github:edolstra/dwarffs").rev
          ```
        )",
      .fun = prim_get_flake,
  };
}

static void prim_parse_flake_ref(eval_state_t& state, const pos_idx_t pos, value_t** args,
                                 value_t& v) {
  std::string flake_ref_s(state.forceStringNoCtx(
      *args[0], pos, "while evaluating the argument passed to builtins.parseFlakeRef"));
  auto attrs = nix::parse_flake_ref(state.fetch_settings, flake_ref_s, {}, true).toAttrs();
  auto binds = state.buildBindings(attrs.size());
  for (const auto& [key, value] : attrs) {
    auto s = state.symbols.create(key);
    auto& vv = binds.alloc(s);
    std::visit(
        overloaded{[&vv, &state](const std::string& value) { vv.mk_string(value, state.mem); },
                   [&vv](const uint64_t& value) { vv.mkInt(value); },
                   [&vv](const explicit_t<bool>& value) { vv.mkBool(value.t_); }},
        value);
  }
  v.mkAttrs(binds);
}

nix::PrimOp parse_flake_ref({
    .name = "__parseFlakeRef",
    .args = {"flake-ref"},
    .doc = R"(
      Parse a flake reference, and return its exploded form.

      For example:

      ```nix
      builtins.parse_flake_ref "github:NixOS/nixpkgs/23.05?dir=lib"
      ```

      evaluates to:

      ```nix
      { dir = "lib"; owner = "NixOS"; ref = "23.05"; repo = "nixpkgs"; type = "github"; }
      ```
    )",
    .fun = prim_parse_flake_ref,
});

static void prim_flake_ref_to_string(eval_state_t& state, const pos_idx_t pos, value_t** args,
                                     value_t& v) {
  state.forceAttrs(*args[0], no_pos,
                   "while evaluating the argument passed to builtins.flakeRefToString");
  fetchers::Attrs attrs;
  for (const auto& attr : *args[0]->attrs()) {
    auto t = attr.value->type();
    if (t == nInt) {
      auto int_value = attr.value->integer().value;

      if (int_value < 0) {
        state
            .error<EvalError>("negative value given for flake ref attr %1%: %2%",
                              state.symbols[attr.name], int_value)
            .at_pos(pos)
            .debugThrow();
      }

      attrs.emplace(state.symbols[attr.name], uint64_t(int_value));
    } else if (t == nBool) {
      attrs.emplace(state.symbols[attr.name], explicit_t<bool>{attr.value->boolean()});
    } else if (t == nString) {
      attrs.emplace(state.symbols[attr.name], std::string(attr.value->string_view()));
    } else {
      state
          .error<EvalError>("flake reference attribute sets may only contain integers, Booleans, "
                            "and strings, but attribute '%s' is %s",
                            state.symbols[attr.name], show_type(*attr.value))
          .debugThrow();
    }
  }
  auto flake_ref = flake_ref_t::fromAttrs(state.fetch_settings, attrs);
  v.mk_string(flake_ref.to_string(), state.mem);
}

nix::PrimOp flake_ref_to_string({
    .name = "__flakeRefToString",
    .args = {"attrs"},
    .doc = R"(
      Convert a flake reference from attribute set format to URL format.

      For example:

      ```nix
      builtins.flake_ref_to_string {
        dir = "lib"; owner = "NixOS"; ref = "23.05"; repo = "nixpkgs"; type = "github";
      }
      ```

      evaluates to

      ```nix
      "github:NixOS/nixpkgs/23.05?dir=lib"
      ```
    )",
    .fun = prim_flake_ref_to_string,
});

} // namespace nix::flake::primops
