#include "nix/expr/eval.h"

#include <nlohmann/json.hpp>

#include "nix/cmd/command-installable-value.h"
#include "nix/expr/eval-inline.h"
#include "nix/expr/value-to-json.h"
#include "nix/main/common-args.h"
#include "nix/main/shared.h"
#include "nix/store/store-api.h"

using namespace nix;

namespace nix::fs {
using namespace std::filesystem;
}

struct cmd_eval_t : MixJSON, InstallableValueCommand, MixReadOnlyOption {
  bool raw = false;
  std::optional<std::string> apply;
  std::optional<std::filesystem::path> write_to;

  cmd_eval_t() : InstallableValueCommand() {
    add_flag({
        .long_name = "raw",
        .description = "Print strings without quotes or escaping.",
        .handler = {&raw, true},
    });

    add_flag({
        .long_name = "apply",
        .description = "Apply the function *expr* to each argument.",
        .labels = {"expr"},
        .handler = {&apply},
    });

    add_flag({
        .long_name = "write-to",
        .description = "Write a string or attrset of strings to *path*.",
        .labels = {"path"},
        .handler = {&write_to},
    });
  }

  std::string description() override { return "evaluate a Nix expression"; }

  std::string doc() override {
    return
#include "eval.md"
        ;
  }

  category_t category() override { return catSecondary; }

  void run(ref<store_t> store, ref<InstallableValue> installable) override {
    if (raw && json)
      throw UsageError("--raw and --json are mutually exclusive");

    auto state = getEvalState();

    auto [v, pos] = installable->toValue(*state);
    NixStringContext context;

    if (apply) {
      auto v_apply = state->allocValue();
      state->eval(state->parseExprFromString(*apply, state->root_path(".")), *v_apply);
      auto v_res = state->allocValue();
      state->callFunction(*v_apply, *v, *v_res, no_pos);
      v = v_res;
    }

    if (write_to) {
      logger->stop();

      if (path_exists(*write_to))
        throw Error("path '%s' already exists", write_to->string());

      [&](this const auto& recurse, value_t& v, const pos_idx_t pos,
          const std::filesystem::path& path) -> void {
        state->forceValue(v, pos);
        if (v.type() == nString)
          // FIXME: disallow strings with contexts?
          write_file(path.string(), v.string_view());
        else if (v.type() == nAttrs) {
          [[maybe_unused]] bool directory_created = std::filesystem::create_directory(path);
          // Directory should not already exist
          assert(directory_created);
          for (auto& attr : *v.attrs()) {
            std::string_view name = state->symbols[attr.name];
            try {
              if (name == "." || name == "..")
                throw Error("invalid file name '%s'", name);
              recurse(*attr.value, attr.pos, path / name);
            } catch (Error& e) {
              e.add_trace(state->positions[attr.pos],
                         hint_fmt_t("while evaluating the attribute '%s'", name));
              throw;
            }
          }
        } else
          state
              ->error<TypeError>("value at '%s' is not a string or an attribute set",
                                 state->positions[pos])
              .debugThrow();
      }(*v, pos, *write_to);
    }

    else if (raw) {
      logger->stop();
      write_full(
          get_standard_output(),
          state->devirtualize(*state->coerceToString(no_pos, *v, context,
                                                     "while generating the eval command output"),
                              context));
    }

    else if (json) {
      // FIXME: use printJSON
      auto j = print_value_as_json(*state, true, *v, pos, context, false);
      logger->cout("%s", state->devirtualize(outputPretty ? j.dump(2) : j.dump(), context));
    }

    else {
      logger->cout("%s",
                   ValuePrinter(*state, *v, PrintOptions{.force = true, .derivationPaths = true}));
    }
  }
};

static auto r_cmd_eval = registerCommand<cmd_eval_t>("eval");
