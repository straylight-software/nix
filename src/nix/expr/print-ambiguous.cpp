#include "nix/expr/print-ambiguous.h"

#include "nix/expr/eval.h"
#include "nix/expr/print.h"
#include "nix/util/signals.h"

namespace nix {

// See: https://github.com/NixOS/nix/issues/9730
void print_ambiguous(eval_state_t& state, value_t& v, std::ostream& str,
                     std::set<const void*>* seen, int depth) {
  check_interrupt();

  if (depth <= 0) {
    str << "«too deep»";
    return;
  }
  switch (v.type()) {
    case nInt:
      str << v.integer();
      break;
    case nBool:
      print_literal_bool(str, v.boolean());
      break;
    case nString: {
      NixStringContext context;
      copy_context(v, context);
      // FIXME: make devirtualization configurable?
      print_literal_string(str, state.devirtualize(v.string_view(), context));
      break;
    }
    case nPath:
      str << v.path().to_string(); // !!! escaping?
      break;
    case nNull:
      str << "null";
      break;
    case nAttrs: {
      if (seen && !v.attrs()->empty() && !seen->insert(v.attrs()).second)
        str << "«repeated»";
      else {
        str << "{ ";
        for (auto& i : v.attrs()->lexicographicOrder(state.symbols)) {
          str << state.symbols[i->name] << " = ";
          print_ambiguous(state, *i->value, str, seen, depth - 1);
          str << "; ";
        }
        str << "}";
      }
      break;
    }
    case nList:
      /* use pointer to the value_t instead of pointer to the elements, because
         that would need to explicitly handle the case of SmallList. */
      if (seen && v.list_size() && !seen->insert(&v).second)
        str << "«repeated»";
      else {
        str << "[ ";
        for (auto v2 : v.list_view()) {
          if (v2)
            print_ambiguous(state, *v2, str, seen, depth - 1);
          else
            str << "(nullptr)";
          str << " ";
        }
        str << "]";
      }
      break;
    case nThunk:
      if (!v.isBlackhole()) {
        str << "<CODE>";
      } else {
        // Although we know for sure that it's going to be an infinite recursion
        // when this value is accessed _in the current context_, it's likely
        // that the user will misinterpret a simpler «infinite recursion» output
        // as a definitive statement about the value, while in fact it may be
        // a valid value after `builtins.trace` and perhaps some other steps
        // have completed.
        str << "«potential infinite recursion»";
      }
      break;
    case nFailed:
      str << "«failed»";
      break;
    case nFunction:
      if (v.isLambda()) {
        str << "<LAMBDA>";
      } else if (v.isPrimOp()) {
        str << "<PRIMOP>";
      } else if (v.isPrimOpApp()) {
        str << "<PRIMOP-APP>";
      }
      break;
    case nExternal:
      str << *v.external();
      break;
    case nFloat:
      str << v.fpoint();
      break;
    default:
      printError("Nix evaluator internal error: printAmbiguous: invalid value type");
      unreachable();
  }
}

} // namespace nix
