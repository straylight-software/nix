#include "nix/expr/eval-inline.h"
#include "nix/expr/primops.h"
#include "nix/store/derivations.h"
#include "nix/store/globals.h"
#include "nix/store/store-api.h"

namespace nix {

static void prim_unsafe_discard_string_context(eval_state_t& state, const pos_idx_t pos,
                                               value_t** args, value_t& v) {
  NixStringContext context, filtered;

  auto s = state.coerceToString(
      pos, *args[0], context,
      "while evaluating the argument passed to builtins.unsafeDiscardStringContext");

  for (auto& c : context) {
    if (auto* p = std::get_if<NixStringContextElem::Path>(&c.raw)) {
      filtered.insert(*p);
    }
  }

  v.mk_string(*s, filtered, state.mem);
}

static RegisterPrimOp primop_unsafe_discard_string_context({
    .name = "__unsafeDiscardStringContext",
    .args = {"s"},
    .doc = R"(
        Discard the [string context](@docroot@/language/string-context.md) from a value that can be coerced to a string.
    )",
    .fun = prim_unsafe_discard_string_context,
});

bool has_context(const NixStringContext& context) {
  for (auto& c : context) {
    if (!std::get_if<NixStringContextElem::Path>(&c.raw)) {
      return true;
    }
  }
  return false;
}

static void prim_has_context(eval_state_t& state, const pos_idx_t pos, value_t** args, value_t& v) {
  NixStringContext context;
  state.forceString(*args[0], context, pos,
                    "while evaluating the argument passed to builtins.hasContext");
  v.mkBool(has_context(context));
}

static RegisterPrimOp primop_has_context({.name = "__hasContext",
                                          .args = {"s"},
                                          .doc = R"(
      Return `true` if string *s* has a non-empty context.
      The context can be obtained with
      [`getContext`](#builtins-getContext).

      > **Example**
      >
      > Many operations require a string context to be empty because they are intended only to work with "regular" strings, and also to help users avoid unintentionally loosing track of string context elements.
      > `builtins.has_context` can help create better domain-specific errors in those case.
      >
      > ```nix
      > name: meta:
      >
      > if builtins.has_context name
      > then throw "package name cannot contain string context"
      > else { ${name} = meta; }
      > ```
    )",
                                          .fun = prim_has_context});

static void prim_unsafe_discard_output_dependency(eval_state_t& state, const pos_idx_t pos,
                                                  value_t** args, value_t& v) {
  NixStringContext context;
  auto s = state.coerceToString(
      pos, *args[0], context,
      "while evaluating the argument passed to builtins.unsafeDiscardOutputDependency");

  NixStringContext context2;
  for (auto&& c : context) {
    if (auto* ptr = std::get_if<NixStringContextElem::DrvDeep>(&c.raw)) {
      state.waitForPath(ptr->drv_path); // FIXME: why?
      context2.emplace(NixStringContextElem::opaque_t{.path = ptr->drv_path});
    } else {
      /* Can reuse original item */
      context2.emplace(std::move(c).raw);
    }
  }

  v.mk_string(*s, context2, state.mem);
}

static RegisterPrimOp
    primop_unsafe_discard_output_dependency({.name = "__unsafeDiscardOutputDependency",
                                             .args = {"s"},
                                             .doc = R"(
      Create a copy of the given string where every
      [derivation deep](@docroot@/language/string-context.md#string-context-element-derivation-deep)
      string context element is turned into a
      [constant](@docroot@/language/string-context.md#string-context-constant)
      string context element.

      This is the opposite of [`builtins.addDrvOutputDependencies`](#builtins-addDrvOutputDependencies).

      This is unsafe because it allows us to "forget" store objects we would have otherwise referred to with the string context,
      whereas Nix normally tracks all dependencies consistently.
      Safe operations "grow" but never "shrink" string contexts.
      [`builtins.addDrvOutputDependencies`] in contrast is safe because "derivation deep" string context element always refers to the underlying derivation (among many more things).
      Replacing a constant string context element with a "derivation deep" element is a safe operation that just enlargens the string context without forgetting anything.

      [`builtins.addDrvOutputDependencies`]: #builtins-addDrvOutputDependencies
    )",
                                             .fun = prim_unsafe_discard_output_dependency});

static void prim_add_drv_output_dependencies(eval_state_t& state, const pos_idx_t pos,
                                             value_t** args, value_t& v) {
  NixStringContext context;
  auto s = state.coerceToString(
      pos, *args[0], context,
      "while evaluating the argument passed to builtins.addDrvOutputDependencies");

  auto context_size = context.size();
  if (context_size != 1) {
    state
        .error<EvalError>("context of string '%s' must have exactly one element, but has %d", *s,
                          context_size)
        .at_pos(pos)
        .debugThrow();
  }
  NixStringContext context2{
      (NixStringContextElem{std::visit(
          overloaded{
              [&](const NixStringContextElem::opaque_t& c) -> NixStringContextElem::DrvDeep {
                if (!c.path.is_derivation()) {
                  state
                      .error<EvalError>("path '%s' is not a derivation",
                                        state.store->printStorePath(c.path))
                      .at_pos(pos)
                      .debugThrow();
                }
                return NixStringContextElem::DrvDeep{
                    .drv_path = c.path,
                };
              },
              [&](const NixStringContextElem::Built& c) -> NixStringContextElem::DrvDeep {
                state
                    .error<EvalError>("`addDrvOutputDependencies` can only act on derivations, not "
                                      "on a derivation output such as '%1%'",
                                      c.output)
                    .at_pos(pos)
                    .debugThrow();
              },
              [&](const NixStringContextElem::DrvDeep& c) -> NixStringContextElem::DrvDeep {
                /* Reuse original item because we want this to be idempotent. */
                /* FIXME: Suspicious move out of const. This is actually a copy, so the comment
                 above does not make much sense. */
                return std::move(c);
              },
              [&](const NixStringContextElem::Path& p) -> NixStringContextElem::DrvDeep {
                state
                    .error<EvalError>(
                        "`addDrvOutputDependencies` does not work on a string without context")
                    .at_pos(pos)
                    .debugThrow();
              },
          },
          context.begin()->raw)}),
  };

  v.mk_string(*s, context2, state.mem);
}

static RegisterPrimOp primop_add_drv_output_dependencies({.name = "__addDrvOutputDependencies",
                                                          .args = {"s"},
                                                          .doc = R"(
      Create a copy of the given string where a single
      [constant](@docroot@/language/string-context.md#string-context-constant)
      string context element is turned into a
      [derivation deep](@docroot@/language/string-context.md#string-context-element-derivation-deep)
      string context element.

      The store path that is the constant string context element should point to a valid derivation, and end in `.drv`.

      The original string context element must not be empty or have multiple elements, and it must not have any other type of element other than a constant or derivation deep element.
      The latter is supported so this function is idempotent.

      This is the opposite of [`builtins.unsafeDiscardOutputDependency`](#builtins-unsafeDiscardOutputDependency).
    )",
                                                          .fun = prim_add_drv_output_dependencies});

/* Extract the context of a string as a structured Nix value.

   The context is represented as an attribute set whose keys are the
   paths in the context set and whose values are attribute sets with
   the following keys:
     path: True if the relevant path is in the context as a plain store
           path (i.e. the kind of context you get when interpolating
           a Nix path (e.g. ./.) into a string). False if missing.
     all_outputs: True if the relevant path is a derivation and it is
                  in the context as a drv file with all of its outputs
                  (i.e. the kind of context you get when referencing
                  .drv_path of some derivation). False if missing.
     outputs: If a non-empty list, the relevant path is a derivation
              and the provided outputs are referenced in the context
              (i.e. the kind of context you get when referencing
              .out_path of some derivation). Empty list if missing.
   Note that for a given path any combination of the above attributes
   may be present.
*/
static void prim_get_context(eval_state_t& state, const pos_idx_t pos, value_t** args, value_t& v) {
  struct context_info {
    bool path = false;
    bool all_outputs = false;
    strings_t outputs;
  };

  NixStringContext context;
  state.forceString(*args[0], context, pos,
                    "while evaluating the argument passed to builtins.getContext");
  auto context_infos = std::map<store_path_t, context_info>();
  for (auto&& i : context) {
    std::visit(overloaded{
                   [&](NixStringContextElem::DrvDeep&& d) {
                     context_infos[std::move(d.drv_path)].all_outputs = true;
                   },
                   [&](NixStringContextElem::Built&& b) {
                     // FIXME should eventually show string context as is, no
                     // resolving here.
                     auto drv_path = resolve_derived_path(*state.store, *b.drv_path);
                     context_infos[std::move(drv_path)].outputs.emplace_back(std::move(b.output));
                   },
                   [&](NixStringContextElem::opaque_t&& o) {
                     context_infos[std::move(o.path)].path = true;
                   },
                   [&](NixStringContextElem::Path&& p) {},
               },
               ((NixStringContextElem&&)i).raw);
  }

  auto attrs = state.buildBindings(context_infos.size());

  auto s_path = state.symbols.create("path");
  auto s_all_outputs = state.symbols.create("allOutputs");
  for (const auto& info : context_infos) {
    auto info_attrs = state.buildBindings(3);
    if (info.second.path) {
      info_attrs.alloc(s_path).mkBool(true);
    }
    if (info.second.all_outputs) {
      info_attrs.alloc(s_all_outputs).mkBool(true);
    }
    if (!info.second.outputs.empty()) {
      auto list = state.buildList(info.second.outputs.size());
      for (const auto& [i, output] : enumerate(info.second.outputs)) {
        (list[i] = state.allocValue())->mk_string(output, state.mem);
      }
      info_attrs.alloc(state.s.outputs).mkList(list);
    }
    attrs.alloc(state.store->printStorePath(info.first)).mkAttrs(info_attrs);
  }

  v.mkAttrs(attrs);
}

static RegisterPrimOp primop_get_context({.name = "__getContext",
                                          .args = {"s"},
                                          .doc = R"(
      Return the string context of *s*.

      The string context tracks references to derivations within a string.
      It is represented as an attribute set of [store derivation](@docroot@/glossary.md#gloss-store-derivation) paths mapping to output names.

      Using [string interpolation](@docroot@/language/string-interpolation.md) on a derivation adds that derivation to the string context.
      For example,

      ```nix
      builtins.getContext "${derivation { name = "a"; builder = "b"; system = "c"; }}"
      ```

      evaluates to

      ```
      { "/nix/store/arhvjaf6zmlyn8vh8fgn55rpwnxq0n7l-a.drv" = { outputs = [ "out" ]; }; }
      ```
    )",
                                          .fun = prim_get_context});

/* Append the given context to a given string.

   See the commentary above getContext for details of the
   context representation.
*/
static void prim_append_context(eval_state_t& state, const pos_idx_t pos, value_t** args,
                                value_t& v) {
  NixStringContext context;
  auto orig =
      state.forceString(*args[0], context, no_pos,
                        "while evaluating the first argument passed to builtins.appendContext");

  state.forceAttrs(*args[1], pos,
                   "while evaluating the second argument passed to builtins.appendContext");

  auto s_path = state.symbols.create("path");
  auto s_all_outputs = state.symbols.create("allOutputs");
  for (auto& i : *args[1]->attrs()) {
    const auto& name = state.symbols[i.name];
    if (!state.store->isStorePath(name)) {
      state.error<EvalError>("context key '%s' is not a store path", name)
          .at_pos(i.pos)
          .debugThrow();
    }
    auto namePath = state.store->parseStorePath(name);
    if (!settings.readOnlyMode) {
      state.store->ensure_path(namePath);
    }
    state.forceAttrs(*i.value, i.pos, "while evaluating the value of a string context");

    if (auto attr = i.value->attrs()->get(s_path)) {
      if (state.forceBool(*attr->value, attr->pos,
                          "while evaluating the `path` attribute of a string context")) {
        context.emplace(NixStringContextElem::opaque_t{
            .path = namePath,
        });
      }
    }

    if (auto attr = i.value->attrs()->get(s_all_outputs)) {
      if (state.forceBool(*attr->value, attr->pos,
                          "while evaluating the `allOutputs` attribute of a string context")) {
        if (!is_derivation(name)) {
          state
              .error<EvalError>(
                  "tried to add all-outputs context of %s, which is not a derivation, to a string",
                  name)
              .at_pos(i.pos)
              .debugThrow();
        }
        context.emplace(NixStringContextElem::DrvDeep{
            .drv_path = namePath,
        });
      }
    }

    if (auto attr = i.value->attrs()->get(state.s.outputs)) {
      state.forceList(*attr->value, attr->pos,
                      "while evaluating the `outputs` attribute of a string context");
      if (attr->value->list_size() && !is_derivation(name)) {
        state
            .error<EvalError>("tried to add derivation output context of %s, which is not a "
                              "derivation, to a string",
                              name)
            .at_pos(i.pos)
            .debugThrow();
      }
      for (auto elem : attr->value->list_view()) {
        auto output_name = state.forceStringNoCtx(
            *elem, attr->pos, "while evaluating an output name within a string context");
        context.emplace(NixStringContextElem::Built{
            .drv_path = makeConstantStorePathRef(namePath),
            .output = std::string{output_name},
        });
      }
    }
  }

  v.mk_string(orig, context, state.mem);
}

static RegisterPrimOp
    primop_append_context({.name = "__appendContext", .arity = 2, .fun = prim_append_context});

} // namespace nix
