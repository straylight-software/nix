// straylight // nix-language // tests
//
// Tests for the straylight::nix::compiler::evaluator with I/O support

// Enable sync I/O for these tests
#define STRAYLIGHT_EVAL_IO_SYNC 1

#include <filesystem>
#include <fstream>
#include <iostream>

#include <catch2/catch_test_macros.hpp>

#include "straylight/nix/compiler/straylight::nix::compiler::evaluator.h"

namespace {

// Helper to create a temporary directory with test files
struct test_directory {
  test_directory() {
    // Create a unique temp directory
    temp_dir_ = std::filesystem::temp_directory_path() / ("nix_test_" + std::to_string(getpid()));
    std::filesystem::create_directories(temp_dir_);
  }

  ~test_directory() {
    std::error_code ec;
    std::filesystem::remove_all(temp_dir_, ec);
  }

  void write_file(const std::string& name, const std::string& content) {
    std::ofstream f(temp_dir_ / name);
    f << content;
  }

  [[nodiscard]] auto path() const -> const std::filesystem::path& { return temp_dir_; }

  [[nodiscard]] auto file_path(const std::string& name) const -> std::filesystem::path {
    return temp_dir_ / name;
  }

private:
  std::filesystem::path temp_dir_;
};

} // namespace

// =============================================================================
// Basic Evaluator Tests
// =============================================================================

TEST_CASE("straylight::nix::compiler::evaluator - basic expressions",
          "[straylight::nix::compiler::evaluator]") {
  straylight::nix::compiler::evaluator eval;

  SECTION("integer") {
    auto result = eval.eval_string("42");
    REQUIRE(result.has_value());
    CHECK(result.value() == "42");
  }

  SECTION("arithmetic") {
    auto result = eval.eval_string("1 + 2 * 3");
    REQUIRE(result.has_value());
    CHECK(result.value() == "7");
  }

  SECTION("string") {
    auto result = eval.eval_string("\"hello\"");
    REQUIRE(result.has_value());
    CHECK(result.value() == "\"hello\"");
  }

  SECTION("attrset") {
    auto result = eval.eval_string("{ a = 1; b = 2; }");
    REQUIRE(result.has_value());
    // format_value returns summary form "{ N attrs }"
    CHECK(result.value() == "{ 2 attrs }");
  }
}

// =============================================================================
// File Evaluation Tests
// =============================================================================

TEST_CASE("straylight::nix::compiler::evaluator - eval_file",
          "[straylight::nix::compiler::evaluator]") {
  test_directory dir;
  straylight::nix::compiler::evaluator eval;

  SECTION("simple file") {
    dir.write_file("test.nix", "1 + 2");
    auto result = eval.eval_file(dir.file_path("test.nix"));
    REQUIRE(result.has_value());
    CHECK(result.value() == "3");
  }

  SECTION("attrset file") {
    dir.write_file("config.nix", "{ foo = 42; bar = \"hello\"; }");
    auto result = eval.eval_file(dir.file_path("config.nix"));
    REQUIRE(result.has_value());
    // format_value returns summary form "{ N attrs }"
    CHECK(result.value() == "{ 2 attrs }");
  }

  SECTION("let expression") {
    dir.write_file("let.nix", "let x = 10; y = 20; in x + y");
    auto result = eval.eval_file(dir.file_path("let.nix"));
    REQUIRE(result.has_value());
    CHECK(result.value() == "30");
  }
}

// =============================================================================
// Import Tests (require sync I/O backend)
// =============================================================================

TEST_CASE("straylight::nix::compiler::evaluator - import",
          "[straylight::nix::compiler::evaluator][import]") {
  test_directory dir;
  straylight::nix::compiler::evaluator eval;

  SECTION("simple import") {
    // Create a file to import
    dir.write_file("lib.nix", "{ x = 42; }");

    // Create main file that imports it
    dir.write_file("main.nix", "(import ./lib.nix).x");

    auto result = eval.eval_file(dir.file_path("main.nix"));
    if (!result.has_value()) {
      std::cerr << "Error: " << result.error().message << "\n";
      std::cerr << "File: " << result.error().file << "\n";
      std::cerr << "Line: " << result.error().line << "\n";
    }
    REQUIRE(result.has_value());
    CHECK(result.value() == "42");
  }

  SECTION("import with function") {
    // Create a library with a function
    dir.write_file("add.nix", "a: b: a + b");

    // Use the function
    dir.write_file("main.nix", "let add = import ./add.nix; in add 10 20");

    auto result = eval.eval_file(dir.file_path("main.nix"));
    if (!result.has_value()) {
      std::cerr << "import with function failed: " << result.error().message << "\n";
    }
    REQUIRE(result.has_value());
    CHECK(result.value() == "30");
  }

  SECTION("nested imports") {
    // Create a chain of imports
    dir.write_file("base.nix", "{ value = 100; }");
    dir.write_file("middle.nix", "let b = import ./base.nix; in { doubled = b.value * 2; }");
    dir.write_file("top.nix", "(import ./middle.nix).doubled");

    auto result = eval.eval_file(dir.file_path("top.nix"));
    if (!result.has_value()) {
      std::cerr << "nested imports failed: " << result.error().message << "\n";
      std::cerr << "File: " << result.error().file << "\n";
    }
    REQUIRE(result.has_value());
    CHECK(result.value() == "200");
  }

  SECTION("import caching - same file imported twice") {
    dir.write_file("shared.nix", "{ counter = 1; }");
    dir.write_file(
        "main.nix",
        "let a = import ./shared.nix; b = import ./shared.nix; in a.counter + b.counter");

    auto result = eval.eval_file(dir.file_path("main.nix"));
    REQUIRE(result.has_value());
    CHECK(result.value() == "2");
  }

  SECTION("directory import - default.nix") {
    // Create a directory with default.nix
    std::filesystem::create_directories(dir.path() / "mylib");
    std::ofstream f(dir.path() / "mylib" / "default.nix");
    f << "{ name = \"mylib\"; version = 1; }";
    f.close();

    dir.write_file("main.nix", "(import ./mylib).name");

    auto result = eval.eval_file(dir.file_path("main.nix"));
    if (!result.has_value()) {
      std::cerr << "directory import failed: " << result.error().message << "\n";
      std::cerr << "File: " << result.error().file << "\n";
    }
    REQUIRE(result.has_value());
    CHECK(result.value() == "\"mylib\"");
  }
}

// =============================================================================
// Import Error Tests
// =============================================================================

TEST_CASE("straylight::nix::compiler::evaluator - import errors",
          "[straylight::nix::compiler::evaluator][import][error]") {
  test_directory dir;
  straylight::nix::compiler::evaluator eval;

  SECTION("import non-existent file") {
    dir.write_file("main.nix", "import ./does-not-exist.nix");

    auto result = eval.eval_file(dir.file_path("main.nix"));
    CHECK(!result.has_value());
    // The error should indicate the file wasn't found
  }

  SECTION("import cycle detection") {
    // Create a cycle: a imports b, b imports a
    dir.write_file("a.nix", "import ./b.nix");
    dir.write_file("b.nix", "import ./a.nix");

    auto result = eval.eval_file(dir.file_path("a.nix"));
    CHECK(!result.has_value());
    // The error should indicate a cycle
  }

  SECTION("import with parse error") {
    dir.write_file("broken.nix", "{ this is not valid nix }}}");
    dir.write_file("main.nix", "import ./broken.nix");

    auto result = eval.eval_file(dir.file_path("main.nix"));
    CHECK(!result.has_value());
  }
}
