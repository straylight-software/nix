#include "nix/expr/value-to-json.h"

#include <cstdlib>
#include <iomanip>

#include <nlohmann/json.hpp>

#include "nix/expr/eval-inline.h"
#include "nix/expr/parallel-eval.h"
#include "nix/store/store-api.h"
#include "nix/util/signals.h"

namespace nix {

using json = nlohmann::json;

#pragma GCC diagnostic ignored "-Wswitch-enum"

static void parallel_force_deep(eval_state_t& state, value_t& v, pos_idx_t pos) {
  state.forceValue(v, pos);

  std::vector<std::pair<Executor::work_t, uint8_t>> work;

  switch (v.type()) {
    case nAttrs: {
      NixStringContext context;
      if (state.tryAttrsToString(pos, v, context, false, false))
        return;
      if (v.attrs()->get(state.s.out_path))
        return;
      for (auto& a : *v.attrs())
        work.emplace_back([value(alloc_root_value(a.value)), pos(a.pos),
                           &state]() { parallel_force_deep(state, **value, pos); },
                          0);
      break;
    }

    default:
      break;
  }

  state.executor->spawn(std::move(work));
}

// TODO: rename. It doesn't print.
json print_value_as_json(eval_state_t& state, bool strict, value_t& v, const pos_idx_t pos,
                      NixStringContext& context, bool copy_to_store) {
  if (strict && state.executor->enabled && !Executor::amWorkerThread)
    parallel_force_deep(state, v, pos);

  auto recurse = [&](this const auto& recurse, json& res, value_t& v, pos_idx_t pos) -> void {
    check_interrupt();

    auto _level = state.addCallDepth(pos);

    if (strict)
      state.forceValue(v, pos);

    switch (v.type()) {
      case nInt:
        res = v.integer().value;
        break;

      case nBool:
        res = v.boolean();
        break;

      case nString: {
        copy_context(v, context);
        res = v.string_view();
        break;
      }

      case nPath:
        if (copy_to_store)
          res = state.store->printStorePath(
              state.copyPathToStore(context, v.path(), v.determinePos(pos)));
        else
          res = v.path().path.abs();
        break;

      case nNull:
        // already initialized as null
        break;

      case nAttrs: {
        auto maybe_string = state.tryAttrsToString(pos, v, context, false, false);
        if (maybe_string) {
          res = *maybe_string;
          break;
        }
        if (auto i = v.attrs()->get(state.s.out_path))
          return recurse(res, *i->value, i->pos);
        else {
          res = json::object();
          for (auto& a : v.attrs()->lexicographicOrder(state.symbols)) {
            json& j = res.emplace(state.symbols[a->name], json()).first.value();
            try {
              recurse(j, *a->value, a->pos);
            } catch (Error& e) {
              e.add_trace(state.positions[a->pos],
                         hint_fmt_t("while evaluating attribute '%1%'", state.symbols[a->name]));
              throw;
            }
          }
        }
        break;
      }

      case nList: {
        res = json::array();
        for (const auto& [i, elem] : enumerate(v.list_view())) {
          try {
            res.push_back(json());
            recurse(res.back(), *elem, pos);
          } catch (Error& e) {
            e.add_trace(state.positions[pos],
                       hint_fmt_t("while evaluating list element at index %1%", i));
            throw;
          }
        }
        break;
      }

      case nExternal: {
        res = v.external()->print_value_as_json(state, strict, context, copy_to_store);
        break;
      }

      case nFloat:
        res = v.fpoint();
        break;

      case nThunk:
      case nFailed:
      case nFunction:
        state.error<TypeError>("cannot convert %1% to JSON", show_type(v))
            .at_pos(v.determinePos(pos))
            .debugThrow();
    }
  };

  json res;

  recurse(res, v, pos);

  return res;
}

void print_value_as_json(eval_state_t& state, bool strict, value_t& v, const pos_idx_t pos, std::ostream& str,
                      NixStringContext& context, bool copy_to_store) {
  try {
    str << print_value_as_json(state, strict, v, pos, context, copy_to_store);
  } catch (nlohmann::json::exception& e) {
    throw JSONSerializationError("JSON serialization error: %s", e.what());
  }
}

json ExternalValueBase::print_value_as_json(eval_state_t& state, bool strict, NixStringContext& context,
                                         bool copy_to_store) const {
  state.error<TypeError>("cannot convert %1% to JSON", show_type()).debugThrow();
}

} // namespace nix
