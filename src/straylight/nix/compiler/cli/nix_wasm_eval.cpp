// straylight // nix-language // cli
//
// nix_wasm_eval - evaluate Nix expressions using the WASM evaluator
//
// Usage:
//   nix_wasm_eval "1 + 2"
//   nix_wasm_eval -e "let x = 10; in x * 2"
//   nix_wasm_eval file.nix
//   echo "1 + 2" | nix_wasm_eval

#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>

#include "straylight/nix/compiler/evaluator.h"

auto eval_expr(std::string_view source) -> int {
  try {
    straylight::nix::compiler::evaluator eval;
    auto result = eval.eval_string(source);

    if (!result) {
      std::cerr << "error: " << result.error().message << "\n";
      return 1;
    }

    std::cout << *result << "\n";
    return 0;

  } catch (const std::exception& e) {
    std::cerr << "error: " << e.what() << "\n";
    return 1;
  }
}

auto eval_file(const std::filesystem::path& path) -> int {
  try {
    straylight::nix::compiler::evaluator eval;
    auto result = eval.eval_file(path);

    if (!result) {
      std::cerr << "error: " << result.error().message << "\n";
      return 1;
    }

    std::cout << *result << "\n";
    return 0;

  } catch (const std::exception& e) {
    std::cerr << "error: " << e.what() << "\n";
    return 1;
  }
}

auto main(int argc, char* argv[]) -> int {
  if (argc > 1) {
    std::string arg1 = argv[1];

    if (arg1 == "-h" || arg1 == "--help") {
      std::cout << "Usage: nix_wasm_eval [-e] <expression>\n";
      std::cout << "       nix_wasm_eval <file.nix>\n";
      std::cout << "       echo '<expr>' | nix_wasm_eval\n";
      return 0;
    }

    if (arg1 == "-e" && argc > 2) {
      return eval_expr(argv[2]);
    }

    // Check if it's a file
    std::filesystem::path path(arg1);
    if (std::filesystem::exists(path)) {
      return eval_file(std::filesystem::absolute(path));
    }

    // Otherwise treat as expression
    return eval_expr(arg1);
  }

  // Read from stdin
  std::string source((std::istreambuf_iterator<char>(std::cin)), std::istreambuf_iterator<char>());

  if (source.empty()) {
    std::cerr << "error: no expression provided\n";
    return 1;
  }

  return eval_expr(source);
}
