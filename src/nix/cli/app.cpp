#include "nix/cmd/command.h"
#include "nix/cmd/installable-derived-path.h"
#include "nix/cmd/installable-value.h"
#include "nix/cmd/installables.h"
#include "nix/expr/eval-cache.h"
#include "nix/expr/eval-inline.h"
#include "nix/store/derivations.h"
#include "nix/store/downstream-placeholder.h"
#include "nix/store/names.h"
#include "nix/store/store-api.h"

namespace nix {

/**
 * Return the rewrites that are needed to resolve a string whose context is
 * included in `dependencies`.
 */
string_pairs_t resolve_rewrites(Store& store, const std::vector<BuiltPathWithResult>& dependencies) {
  string_pairs_t res;
  if (!experimental_feature_settings.is_enabled(xp_t::ca_derivations)) {
    return res;
  }
  for (auto& dep : dependencies) {
    auto drvDep = std::get_if<BuiltPathBuilt>(&dep.path);
    if (!drvDep) {
      continue;
    }

    for (const auto& [output_name, output_path] : drvDep->outputs) {
      res.emplace(
          DownstreamPlaceholder::fromSingleDerivedPathBuilt(
              SingleDerivedPath::Built{
                  .drv_path = make_ref<SingleDerivedPath>(drvDep->drv_path->discardOutputPath()),
                  .output = output_name,
              })
              .render(),
          store.printStorePath(output_path));
    }
  }
  return res;
}

/**
 * Resolve the given string assuming the given context.
 */
std::string resolve_string(Store& store, const std::string& to_resolve,
                          const std::vector<BuiltPathWithResult>& dependencies) {
  auto rewrites = resolve_rewrites(store, dependencies);
  return rewrite_strings(to_resolve, rewrites);
}

UnresolvedApp InstallableValue::toApp(EvalState& state) {
  auto cursor = getCursor(state);
  auto attr_path = cursor->getAttrPath();

  auto type = cursor->get_attr("type")->get_string();

  std::string expected_type = !attr_path.empty() && (state.symbols[attr_path[0]] == "apps" ||
                                                   state.symbols[attr_path[0]] == "defaultApp")
                                 ? "app"
                                 : "derivation";
  if (type != expected_type)
    throw Error("attribute '%s' should have type '%s'", cursor->getAttrPathStr(), expected_type);

  if (type == "app") {
    auto [program, context] = cursor->get_attr("program")->getStringWithContext();

    std::vector<DerivedPath> context2;
    for (auto& c : context) {
      context2.emplace_back(std::visit(
          overloaded{
              [&](const NixStringContextElem::DrvDeep& d) -> DerivedPath {
                state.waitForPath(d.drv_path);
                /* We want all outputs of the drv */
                return DerivedPath::Built{
                    .drv_path = makeConstantStorePathRef(d.drv_path),
                    .outputs = OutputsSpec::All{},
                };
              },
              [&](const NixStringContextElem::Built& b) -> DerivedPath {
                state.waitForPath(*b.drv_path);
                return DerivedPath::Built{
                    .drv_path = b.drv_path,
                    .outputs = OutputsSpec::Names{b.output},
                };
              },
              [&](const NixStringContextElem::opaque_t& o) -> DerivedPath {
                return DerivedPath::opaque_t{
                    .path = o.path,
                };
              },
              [&](const NixStringContextElem::Path& p) -> DerivedPath {
                throw Error("'program' attribute of an 'app' output cannot have no context");
              },
          },
          c.raw));
    }

    return UnresolvedApp{App{
        .context = std::move(context2),
        .program = program,
    }};
  }

  else if (type == "derivation") {
    auto drv_path = cursor->forceDerivation();
    auto out_path = cursor->get_attr(state.s.out_path)->get_string();
    auto output_name = cursor->get_attr(state.s.output_name)->get_string();
    auto name = cursor->get_attr(state.s.name)->get_string();
    auto aPname = cursor->maybeGetAttr("pname");
    auto aMeta = cursor->maybeGetAttr(state.s.meta);
    auto aMainProgram = aMeta ? aMeta->maybeGetAttr("mainProgram") : nullptr;
    auto mainProgram = aMainProgram ? aMainProgram->get_string()
                       : aPname     ? aPname->get_string()
                                    : DrvName(name).name;
    auto program = out_path + "/bin/" + mainProgram;
    return UnresolvedApp{App{
        .context = {DerivedPath::Built{
            .drv_path = makeConstantStorePathRef(drv_path),
            .outputs = OutputsSpec::Names{output_name},
        }},
        .program = program,
    }};
  }

  else
    throw Error("attribute '%s' has unsupported type '%s'", cursor->getAttrPathStr(), type);
}

std::vector<BuiltPathWithResult> UnresolvedApp::build(ref<Store> eval_store, ref<Store> store) {
  Installables installableContext;

  for (auto& ctxElt : unresolved.context)
    installableContext.push_back(make_ref<InstallableDerivedPath>(store, DerivedPath{ctxElt}));

  return Installable::build(eval_store, store, Realise::Outputs, installableContext);
}

// FIXME: move to libcmd
App UnresolvedApp::resolve(ref<Store> eval_store, ref<Store> store) {
  auto res = unresolved;

  auto builtContext = build(eval_store, store);
  res.program = resolve_string(*store, unresolved.program.string(), builtContext);
  if (!store->isInStore(res.program.string()))
    throw Error("app program '%s' is not in the Nix store", res.program.string());

  return res;
}

} // namespace nix
