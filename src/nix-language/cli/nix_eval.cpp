// src/nix-language/cli/nix_eval.cpp
//
// Simple CLI for evaluating Nix expressions using the tree-walking interpreter.
//
// Usage:
//   nix_eval -e 'expression'     Evaluate expression from command line
//   nix_eval file.nix            Evaluate file
//   nix_eval                     Read from stdin (REPL mode)

#include <cstring>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>

#include "nix-language/ast/symbol_table.hh"
#include "nix-language/eval/eval.hh"
#include "nix-language/parse/parser.hh"

namespace ast = nix::language::ast;
namespace parse = nix::language::parse;
namespace eval = nix::language::eval;

void print_usage(const char* program) {
  std::cerr << "Usage:\n";
  std::cerr << "  " << program << " -e 'expression'   Evaluate expression\n";
  std::cerr << "  " << program << " file.nix          Evaluate file\n";
  std::cerr << "  " << program << "                   REPL mode (read from stdin)\n";
}

auto evaluate_source(const std::string& source, ast::symbol_table& symbols) -> int {
  try {
    auto expr = parse::parse(source, symbols);
    eval::evaluator evaluator(symbols);
    auto result = evaluator.eval(expr);
    auto forced = evaluator.force(result);
    std::cout << evaluator.print_value(forced) << "\n";
    return 0;
  } catch (const parse::parse_error& e) {
    std::cerr << "Parse error: " << e.what() << "\n";
    return 1;
  } catch (const eval::eval_error& e) {
    std::cerr << "Eval error: " << e.what() << "\n";
    return 1;
  } catch (const std::exception& e) {
    std::cerr << "Error: " << e.what() << "\n";
    return 1;
  }
}

auto read_file(const std::string& path) -> std::string {
  std::ifstream file(path);
  if (!file) {
    throw std::runtime_error("Cannot open file: " + path);
  }
  std::stringstream buffer;
  buffer << file.rdbuf();
  return buffer.str();
}

auto run_repl(ast::symbol_table& symbols) -> int {
  std::cout << "Nix evaluator (type Ctrl-D to exit)\n";
  std::string line;
  eval::evaluator evaluator(symbols);

  while (true) {
    std::cout << "nix> " << std::flush;
    if (!std::getline(std::cin, line)) {
      std::cout << "\n";
      break;
    }

    if (line.empty()) {
      continue;
    }

    try {
      auto expr = parse::parse(line, symbols);
      auto result = evaluator.eval(expr);
      auto forced = evaluator.force(result);
      std::cout << evaluator.print_value(forced) << "\n";
    } catch (const parse::parse_error& e) {
      std::cerr << "Parse error: " << e.what() << "\n";
    } catch (const eval::eval_error& e) {
      std::cerr << "Eval error: " << e.what() << "\n";
    } catch (const std::exception& e) {
      std::cerr << "Error: " << e.what() << "\n";
    }
  }

  return 0;
}

auto main(int argc, char** argv) -> int {
  ast::symbol_table symbols;

  if (argc == 1) {
    // REPL mode
    return run_repl(symbols);
  }

  if (argc == 2) {
    // File mode
    const char* arg = argv[1];

    if (std::strcmp(arg, "-h") == 0 || std::strcmp(arg, "--help") == 0) {
      print_usage(argv[0]);
      return 0;
    }

    try {
      std::string source = read_file(arg);
      return evaluate_source(source, symbols);
    } catch (const std::exception& e) {
      std::cerr << "Error: " << e.what() << "\n";
      return 1;
    }
  }

  if (argc == 3 && std::strcmp(argv[1], "-e") == 0) {
    // Expression mode
    return evaluate_source(argv[2], symbols);
  }

  print_usage(argv[0]);
  return 1;
}
