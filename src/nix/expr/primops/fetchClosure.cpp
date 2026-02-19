#include "nix/expr/primops.h"
#include "nix/store/make-content-addressed.h"
#include "nix/store/realisation.h"
#include "nix/store/store-open.h"
#include "nix/util/environment-variables.h"
#include "nix/util/url.h"

namespace nix {

/**
 * Handler for the content addressed case.
 *
 * @param state Evaluator state and store to write to.
 * @param from_store Store containing the path to rewrite.
 * @param from_path Source path to be rewritten.
 * @param to_path_maybe Path to write the rewritten path to. If empty, the error shows the actual
 * path.
 * @param v Return `Value`
 */
static void run_fetch_closure_with_rewrite(EvalState& state, const pos_idx_t pos, Store& from_store,
                                       const StorePath& from_path,
                                       const std::optional<StorePath>& to_path_maybe, Value& v) {
  // establish toPath or throw

  if (!to_path_maybe || !state.store->isValidPath(*to_path_maybe)) {
    auto rewritten_path = make_content_addressed(from_store, *state.store, from_path);
    if (to_path_maybe && *to_path_maybe != rewritten_path)
      throw Error(
          {.msg = hint_fmt_t(
               "rewriting '%s' to content-addressed form yielded '%s', while '%s' was expected",
               state.store->printStorePath(from_path), state.store->printStorePath(rewritten_path),
               state.store->printStorePath(*to_path_maybe)),
           .pos = state.positions[pos]});
    if (!to_path_maybe)
      throw Error(
          {.msg = hint_fmt_t("rewriting '%s' to content-addressed form yielded '%s'\n"
                          "Use this value for the 'toPath' attribute passed to 'fetchClosure'",
                          state.store->printStorePath(from_path),
                          state.store->printStorePath(rewritten_path)),
           .pos = state.positions[pos]});
  }

  const auto& to_path = *to_path_maybe;

  // check and return

  auto result_info = state.store->queryPathInfo(to_path);

  if (!result_info->isContentAddressed(*state.store)) {
    // We don't perform the rewriting when outPath already exists, as an optimisation.
    // However, we can quickly detect a mistake if the toPath is input addressed.
    throw Error({.msg = hint_fmt_t("The 'toPath' value '%s' is input-addressed, so it can't possibly "
                                "be the result of rewriting to a content-addressed path.\n\n"
                                "Set 'toPath' to an empty string to make Nix report the correct "
                                "content-addressed path.",
                                state.store->printStorePath(to_path)),
                 .pos = state.positions[pos]});
  }

  state.allowClosure(to_path);

  state.mkStorePathString(to_path, v);
}

/**
 * Fetch the closure and make sure it's content addressed.
 */
static void run_fetch_closure_with_content_addressed_path(EvalState& state, const pos_idx_t pos,
                                                    Store& from_store, const StorePath& from_path,
                                                    Value& v) {
  if (!state.store->isValidPath(from_path))
    copy_closure(from_store, *state.store, RealisedPath::Set{from_path});

  auto info = state.store->queryPathInfo(from_path);

  if (!info->isContentAddressed(*state.store)) {
    throw Error({.msg = hint_fmt_t("The 'fromPath' value '%s' is input-addressed, but "
                                "'inputAddressed' is set to 'false' (default).\n\n"
                                "If you do intend to fetch an input-addressed store path, add\n\n"
                                "    inputAddressed = true;\n\n"
                                "to the 'fetchClosure' arguments.\n\n"
                                "Note that to ensure authenticity input-addressed store paths, "
                                "users must configure a trusted binary cache public key on their "
                                "systems. This is not needed for content-addressed paths.",
                                state.store->printStorePath(from_path)),
                 .pos = state.positions[pos]});
  }

  state.allowClosure(from_path);

  state.mkStorePathString(from_path, v);
}

/**
 * Fetch the closure and make sure it's input addressed.
 */
static void run_fetch_closure_with_input_addressed_path(EvalState& state, const pos_idx_t pos,
                                                  Store& from_store, const StorePath& from_path,
                                                  Value& v) {
  if (!state.store->isValidPath(from_path))
    copy_closure(from_store, *state.store, RealisedPath::Set{from_path});

  auto info = state.store->queryPathInfo(from_path);

  if (info->isContentAddressed(*state.store)) {
    throw Error({.msg = hint_fmt_t("The store object referred to by 'fromPath' at '%s' is not "
                                "input-addressed, but 'inputAddressed' is set to 'true'.\n\n"
                                "Remove the 'inputAddressed' attribute (it defaults to 'false') to "
                                "expect 'fromPath' to be content-addressed",
                                state.store->printStorePath(from_path)),
                 .pos = state.positions[pos]});
  }

  state.allowClosure(from_path);

  state.mkStorePathString(from_path, v);
}

typedef std::optional<StorePath> store_path_or_gap_t;

static void prim_fetch_closure(EvalState& state, const pos_idx_t pos, Value** args, Value& v) {
  state.forceAttrs(*args[0], pos, "while evaluating the argument passed to builtins.fetchClosure");

  std::optional<std::string> fromStoreUrl;
  std::optional<StorePath> from_path;
  std::optional<store_path_or_gap_t> to_path;
  std::optional<bool> inputAddressedMaybe;

  for (auto& attr : *args[0]->attrs()) {
    std::string_view attr_name = state.symbols[attr.name];
    auto attrHint = [&]() -> std::string {
      return fmt("while evaluating the attribute '%s' passed to builtins.fetchClosure", attr_name);
    };

    if (attr_name == "fromPath") {
      NixStringContext context;
      from_path = state.coerceToStorePath(attr.pos, *attr.value, context, attrHint());
    }

    else if (attr_name == "toPath") {
      state.forceValue(*attr.value, attr.pos);
      bool isEmptyString = attr.value->type() == nString && attr.value->string_view() == "";
      if (isEmptyString) {
        to_path = store_path_or_gap_t{};
      } else {
        NixStringContext context;
        to_path = state.coerceToStorePath(attr.pos, *attr.value, context, attrHint());
      }
    }

    else if (attr_name == "fromStore")
      fromStoreUrl = state.forceStringNoCtx(*attr.value, attr.pos, attrHint());

    else if (attr_name == "inputAddressed")
      inputAddressedMaybe = state.forceBool(*attr.value, attr.pos, attrHint());

    else
      throw Error(
          {.msg = hint_fmt_t("attribute '%s' isn't supported in call to 'fetchClosure'", attr_name),
           .pos = state.positions[pos]});
  }

  if (!from_path)
    throw Error({.msg = hint_fmt_t("attribute '%s' is missing in call to 'fetchClosure'", "fromPath"),
                 .pos = state.positions[pos]});

  bool input_addressed = inputAddressedMaybe.value_or(false);

  if (input_addressed) {
    if (to_path)
      throw Error(
          {.msg = hint_fmt_t(
               "attribute '%s' is set to true, but '%s' is also set. Please remove one of them",
               "inputAddressed", "toPath"),
           .pos = state.positions[pos]});
  }

  if (!fromStoreUrl)
    throw Error({.msg = hint_fmt_t("attribute '%s' is missing in call to 'fetchClosure'", "fromStore"),
                 .pos = state.positions[pos]});

  auto parsed_url = parse_url(*fromStoreUrl, /*lenient=*/true);

  if (parsed_url.scheme != "http" && parsed_url.scheme != "https" &&
      !(get_env("_NIX_IN_TEST").has_value() && parsed_url.scheme == "file"))
    throw Error({.msg = hint_fmt_t("'fetchClosure' only supports http:// and https:// stores"),
                 .pos = state.positions[pos]});

  if (!parsed_url.query.empty())
    throw Error({.msg = hint_fmt_t("'fetchClosure' does not support URL query parameters (in '%s')",
                                *fromStoreUrl),
                 .pos = state.positions[pos]});

  auto from_store = open_store(parsed_url.to_string());

  if (to_path)
    run_fetch_closure_with_rewrite(state, pos, *from_store, *from_path, *to_path, v);
  else if (input_addressed)
    run_fetch_closure_with_input_addressed_path(state, pos, *from_store, *from_path, v);
  else
    run_fetch_closure_with_content_addressed_path(state, pos, *from_store, *from_path, v);
}

static RegisterPrimOp primop_fetch_closure({
    .name = "__fetchClosure",
    .args = {"args"},
    .doc = R"(
      Fetch a store path [closure](@docroot@/glossary.md#gloss-closure) from a binary cache, and return the store path as a string with context.

      This function can be invoked in three ways that we will discuss in order of preference.

      **Fetch a content-addressed store path**

      Example:

      ```nix
      builtins.fetchClosure {
        from_store = "https://cache.nixos.org";
        from_path = /nix/store/ldbhlwhh39wha58rm61bkiiwm6j7211j-git-2.33.1;
      }
      ```

      This is the simplest invocation, and it does not require the user of the expression to configure [`trusted-public-keys`](@docroot@/command-ref/conf-file.md#conf-trusted-public-keys) to ensure their authenticity.

      If your store path is [input addressed](@docroot@/glossary.md#gloss-input-addressed-store-object) instead of content addressed, consider the other two invocations.

      **Fetch any store path and rewrite it to a fully content-addressed store path**

      Example:

      ```nix
      builtins.fetchClosure {
        from_store = "https://cache.nixos.org";
        from_path = /nix/store/nph9br6y2dmciy6q3dj3fwk2brdlr4gh-git-2.33.1;
        to_path = /nix/store/ldbhlwhh39wha58rm61bkiiwm6j7211j-git-2.33.1;
      }
      ```

      This example fetches `/nix/store/r2jd...` from the specified binary cache,
      and rewrites it into the content-addressed store path
      `/nix/store/ldbh...`.

      Like the previous example, no extra configuration or privileges are required.

      To find out the correct value for `to_path` given a `from_path`,
      use [`nix store make-content-addressed`](@docroot@/command-ref/new-cli/nix3-store-make-content-addressed.md):

      ```console
      # nix store make-content-addressed --from https://cache.nixos.org /nix/store/nph9br6y2dmciy6q3dj3fwk2brdlr4gh-git-2.33.1
      rewrote '/nix/store/nph9br6y2dmciy6q3dj3fwk2brdlr4gh-git-2.33.1' to '/nix/store/ldbhlwhh39wha58rm61bkiiwm6j7211j-git-2.33.1'
      ```

      Alternatively, set `to_path = ""` and find the correct `to_path` in the error message.

      **Fetch an input-addressed store path as is**

      Example:

      ```nix
      builtins.fetchClosure {
        from_store = "https://cache.nixos.org";
        from_path = /nix/store/nph9br6y2dmciy6q3dj3fwk2brdlr4gh-git-2.33.1;
        input_addressed = true;
      }
      ```

      It is possible to fetch an [input-addressed store path](@docroot@/glossary.md#gloss-input-addressed-store-object) and return it as is.
      However, this is the least preferred way of invoking `fetchClosure`, because it requires that the input-addressed paths are trusted by the Nix configuration.

      **`builtins.store_path`**

      `fetchClosure` is similar to [`builtins.store_path`](#builtins-store_path) in that it allows you to use a previously built store path in a Nix expression.
      However, `fetchClosure` is more reproducible because it specifies a binary cache from which the path can be fetched.
      Also, using content-addressed store paths does not require users to configure [`trusted-public-keys`](@docroot@/command-ref/conf-file.md#conf-trusted-public-keys) to ensure their authenticity.
    )",
    .fun = prim_fetch_closure,
    .experimental_feature = xp_t::fetch_closure,
});

} // namespace nix
