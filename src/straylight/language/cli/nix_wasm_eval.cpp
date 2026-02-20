// straylight // nix-language // cli
//
// nix_wasm_eval - evaluate Nix expressions using the WASM executor
//
// Usage:
//   nix_wasm_eval "1 + 2"
//   nix_wasm_eval -e "let x = 10; in x * 2"
//   echo "1 + 2" | nix_wasm_eval

#include <cstdlib>
#include <cstring>
#include <iostream>
#include <sstream>
#include <string>

#include "straylight/language/compile/compiler.h"
#include "straylight/language/compile/wasm_types.h"
#include "straylight/language/parse/parser.h"
#include "straylight/language/runtime/wasm_executor.h"

using namespace straylight::language;

auto eval_and_print(std::string_view source) -> int {
  try {
    // Parse
    ast::symbol_table symbols;
    auto expr = parse::parse(source, symbols);

    // Compile
    compile::compiler comp(symbols);
    auto module = comp.compile(expr);

    if (!module.validate()) {
      std::cerr << "error: WASM validation failed\n";
      return 1;
    }

    // Execute
    runtime::wasm_executor executor;
    auto result = executor.execute(module.emit_binary());

    if (!result.success) {
      std::cerr << "error: " << result.error << "\n";
      return 1;
    }

    // Print result
    std::cout << executor.format_value(result.value) << "\n";
    return 0;

  } catch (const std::exception& e) {
    std::cerr << "error: " << e.what() << "\n";
    return 1;
  }
}

auto main(int argc, char* argv[]) -> int {
  std::string source;

  if (argc > 1) {
    // Expression from command line
    std::string arg1 = argv[1];
    if (arg1 == "-e" && argc > 2) {
      source = argv[2];
    } else if (arg1 == "-h" || arg1 == "--help") {
      std::cout << "Usage: nix_wasm_eval [-e] <expression>\n";
      std::cout << "       echo '<expr>' | nix_wasm_eval\n";
      return 0;
    } else {
      source = arg1;
    }
  } else {
    // Read from stdin
    std::ostringstream ss;
    ss << std::cin.rdbuf();
    source = ss.str();
  }

  if (source.empty()) {
    std::cerr << "error: no expression provided\n";
    return 1;
  }

  return eval_and_print(source);
}
