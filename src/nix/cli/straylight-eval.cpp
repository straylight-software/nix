// straylight-eval.cpp - nix eval using straylight WASM evaluator
//
// This command uses the straylight evaluator when STRAYLIGHT_EVAL is defined,
// otherwise falls back to legacy nix evaluation.

#include "straylight/nix/core/component.h"

#include "nix/cmd/command.h"
#include "nix/main/common-args.h"
#include "nix/main/shared.h"
#include "nix/store/store-api.h"
#include "nix/util/logging.h"

#if STRAYLIGHT_EVAL
#  include "straylight/nix/adapters/eval_adapter.h"
#endif

namespace {


struct cmd_straylight_eval : nix::StoreCommand {
  std::string expr_to_eval;
  bool raw = false;

  cmd_straylight_eval() {
    expect_arg("expr", &expr_to_eval);

    add_flag({
        .long_name = "raw",
        .description = "Print result without formatting",
        .handler = {&raw, true},
    });
  }

  std::string description() override { return "evaluate a Nix expression using straylight"; }

  std::string doc() override {
    return R"(
# Examples

Evaluate simple expressions:

```console
$ nix straylight-eval '1 + 2'
3

$ nix straylight-eval '{ x = 1; y = 2; }.x'
1

$ nix straylight-eval 'let f = x: x * 2; in f 21'
42
```

# Description

This command evaluates a Nix expression using the straylight WASM-based
evaluator. It compiles Nix to WebAssembly and executes it, which can be
faster for certain workloads.

Note: Not all Nix features are supported yet. Complex builtins and
import operations may not work.
)";
  }

  nix::category_t category() override { return nix::catUtility; }

  void run(nix::ref<nix::store_t> store) override {
    if constexpr (straylight::nix::core::use_straylight_eval) {
      straylight::nix::adapters::eval_adapter adapter;
      auto result = adapter.eval_string(expr_to_eval);

      if (!result) {
        auto& err = result.error();
        throw nix::Error("%s at %s:%d:%d", err.message, err.file, err.line, err.column);
      }

      if (raw) {
        nix::logger->cout("%s", *result);
      } else {
        nix::logger->cout("%s", *result);
      }
    } else {
      throw nix::Error("straylight evaluator not compiled in\n"
                       "rebuild with -DSTRAYLIGHT_EVAL=1 to enable");
    }
  }
};

static auto r_cmd = nix::registerCommand2<cmd_straylight_eval>({"straylight-eval"});

} // namespace
