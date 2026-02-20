#include "nix/expr/eval-inline.h"
#include "nix/expr/eval-settings.h"
#include "nix/expr/primops.h"
#include "nix/fetchers/fetchers.h"
#include "nix/store/store-api.h"
#include "nix/util/url-parts.h"
#include "nix/util/url.h"

namespace nix {

static void prim_fetch_mercurial(eval_state_t& state, const pos_idx_t pos, value_t** args,
                                 value_t& v) {
  std::string url;
  std::optional<Hash> rev;
  std::optional<std::string> ref;
  std::string_view name = "source";
  NixStringContext context;

  state.forceValue(*args[0], pos);

  if (args[0]->type() == nAttrs) {
    for (auto& attr : *args[0]->attrs()) {
      std::string_view n(state.symbols[attr.name]);
      if (n == "url")
        url = state
                  .coerceToString(
                      attr.pos, *attr.value, context,
                      "while evaluating the `url` attribute passed to builtins.fetchMercurial",
                      false, false)
                  .to_owned();
      else if (n == "rev") {
        // Ugly: unlike fetchGit, here the "rev" attribute can
        // be both a revision or a branch/tag name.
        auto value = state.forceStringNoCtx(
            *attr.value, attr.pos,
            "while evaluating the `rev` attribute passed to builtins.fetchMercurial");
        if (std::regex_match(value.begin(), value.end(), rev_regex))
          rev = Hash::parse_any(value, hash_algorithm_t::SHA1);
        else
          ref = value;
      } else if (n == "name")
        name = state.forceStringNoCtx(
            *attr.value, attr.pos,
            "while evaluating the `name` attribute passed to builtins.fetchMercurial");
      else
        state
            .error<EvalError>("unsupported argument '%s' to 'fetchMercurial'",
                              state.symbols[attr.name])
            .at_pos(attr.pos)
            .debugThrow();
    }

    if (url.empty())
      state.error<EvalError>("'url' argument required").at_pos(pos).debugThrow();

  } else
    url =
        state
            .coerceToString(pos, *args[0], context,
                            "while evaluating the first argument passed to builtins.fetchMercurial",
                            false, false)
            .to_owned();

  // FIXME: git externals probably can be used to bypass the URI
  // whitelist. Ah well.
  state.checkURI(url);

  if (state.settings.pureEval && !rev)
    throw Error("in pure evaluation mode, 'fetchMercurial' requires a Mercurial revision");

  fetchers::Attrs attrs;
  attrs.insert_or_assign("type", "hg");
  attrs.insert_or_assign("url", url.find("://") != std::string::npos ? url : "file://" + url);
  attrs.insert_or_assign("name", std::string(name));
  if (ref)
    attrs.insert_or_assign("ref", *ref);
  if (rev)
    attrs.insert_or_assign("rev", rev->git_rev());
  auto input = fetchers::input_t::fromAttrs(state.fetch_settings, std::move(attrs));

  auto [store_path, accessor, input2] = input.fetch_to_store(state.fetch_settings, *state.store);

  auto attrs2 = state.buildBindings(8);
  state.mkStorePathString(store_path, attrs2.alloc(state.s.out_path));
  if (input2.getRef())
    attrs2.alloc("branch").mk_string(*input2.getRef(), state.mem);
  // Backward compatibility: set 'rev' to
  // 0000000000000000000000000000000000000000 for a dirty tree.
  auto rev2 = input2.getRev().value_or(Hash(hash_algorithm_t::SHA1));
  attrs2.alloc("rev").mk_string(rev2.git_rev(), state.mem);
  attrs2.alloc("shortRev").mk_string(rev2.git_rev().substr(0, 12), state.mem);
  if (auto rev_count = input2.get_rev_count())
    attrs2.alloc("revCount").mkInt(*rev_count);
  v.mkAttrs(attrs2);

  state.allowPath(store_path);
}

static RegisterPrimOp
    r_fetch_mercurial({.name = "fetchMercurial", .arity = 1, .fun = prim_fetch_mercurial});

} // namespace nix
