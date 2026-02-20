// straylight // nix-language // tests
//
// Unit tests for PEGTL grammar rules
// Tests grammar structure without building full AST

#include <string>
#include <string_view>

#include <catch2/catch_test_macros.hpp>
#include <tao/pegtl.hpp>
#include <tao/pegtl/contrib/analyze.hpp>

#include "straylight/language/parse/grammar.h"

namespace p = tao::pegtl;
namespace g = straylight::language::parse::grammar;

// =============================================================================
// helper: test if a rule matches input exactly
// =============================================================================

template <typename Rule>
bool matches_exactly(std::string_view input) {
  p::memory_input in(input, "test");
  try {
    return p::parse<p::must<Rule, p::eof>>(in);
  } catch (const p::parse_error&) {
    return false;
  }
}

template <typename Rule>
bool matches_prefix(std::string_view input) {
  p::memory_input in(input, "test");
  try {
    return p::parse<Rule>(in);
  } catch (const p::parse_error&) {
    return false;
  }
}

// =============================================================================
// grammar analysis
// =============================================================================

TEST_CASE("grammar has no issues", "[grammar][analysis]") {
  // this verifies there are no infinite loops or other structural issues
  auto issues = p::analyze<g::root>();
  REQUIRE(issues == 0);
}

// =============================================================================
// integer literal tests
// =============================================================================

TEST_CASE("integer literal parsing", "[grammar][token][integer]") {
  SECTION("simple integers") {
    REQUIRE(matches_exactly<g::token::integer>("0"));
    REQUIRE(matches_exactly<g::token::integer>("1"));
    REQUIRE(matches_exactly<g::token::integer>("42"));
    REQUIRE(matches_exactly<g::token::integer>("123456789"));
  }

  SECTION("multi-digit integers") {
    REQUIRE(matches_exactly<g::token::integer>("10"));
    REQUIRE(matches_exactly<g::token::integer>("100"));
    REQUIRE(matches_exactly<g::token::integer>("999"));
  }

  SECTION("leading zeros") {
    // Nix allows leading zeros - they're parsed as a single integer
    // (the leading zeros are ignored and the value is the remaining digits)
    REQUIRE(matches_exactly<g::token::integer>("00"));
    REQUIRE(matches_exactly<g::token::integer>("007"));
    REQUIRE(matches_exactly<g::token::integer>("0123"));
  }

  SECTION("negative numbers are not single tokens") {
    // minus is a separate operator
    REQUIRE_FALSE(matches_exactly<g::token::integer>("-1"));
  }
}

// =============================================================================
// float literal tests
// =============================================================================

TEST_CASE("float literal parsing", "[grammar][token][float]") {
  SECTION("simple floats") {
    REQUIRE(matches_exactly<g::token::floating>("1.0"));
    REQUIRE(matches_exactly<g::token::floating>("3.14"));
    REQUIRE(matches_exactly<g::token::floating>("0.5"));
  }

  SECTION("floats with exponents") {
    REQUIRE(matches_exactly<g::token::floating>("1.0e10"));
    REQUIRE(matches_exactly<g::token::floating>("1.0E10"));
    REQUIRE(matches_exactly<g::token::floating>("1.5e+5"));
    REQUIRE(matches_exactly<g::token::floating>("2.0e-3"));
  }

  SECTION("leading dot floats") {
    REQUIRE(matches_exactly<g::token::floating>(".5"));
    REQUIRE(matches_exactly<g::token::floating>(".123"));
  }

  SECTION("invalid floats") {
    REQUIRE_FALSE(matches_exactly<g::token::floating>("1.")); // no trailing dot alone
    REQUIRE_FALSE(matches_exactly<g::token::floating>("1"));  // that's an int
  }
}

// =============================================================================
// identifier tests
// =============================================================================

TEST_CASE("identifier parsing", "[grammar][token][identifier]") {
  SECTION("simple identifiers") {
    REQUIRE(matches_exactly<g::token::identifier>("x"));
    REQUIRE(matches_exactly<g::token::identifier>("foo"));
    REQUIRE(matches_exactly<g::token::identifier>("bar123"));
    REQUIRE(matches_exactly<g::token::identifier>("_private"));
  }

  SECTION("identifiers with hyphens and primes") {
    REQUIRE(matches_exactly<g::token::identifier>("foo-bar"));
    REQUIRE(matches_exactly<g::token::identifier>("x'"));
    REQUIRE(matches_exactly<g::token::identifier>("don't"));
  }

  SECTION("keywords are not identifiers") {
    REQUIRE_FALSE(matches_exactly<g::token::identifier>("if"));
    REQUIRE_FALSE(matches_exactly<g::token::identifier>("then"));
    REQUIRE_FALSE(matches_exactly<g::token::identifier>("else"));
    REQUIRE_FALSE(matches_exactly<g::token::identifier>("let"));
    REQUIRE_FALSE(matches_exactly<g::token::identifier>("in"));
    REQUIRE_FALSE(matches_exactly<g::token::identifier>("with"));
    REQUIRE_FALSE(matches_exactly<g::token::identifier>("assert"));
    REQUIRE_FALSE(matches_exactly<g::token::identifier>("rec"));
    REQUIRE_FALSE(matches_exactly<g::token::identifier>("inherit"));
    REQUIRE_FALSE(matches_exactly<g::token::identifier>("or"));
  }

  SECTION("keyword prefixes are valid identifiers") {
    REQUIRE(matches_exactly<g::token::identifier>("if_"));
    REQUIRE(matches_exactly<g::token::identifier>("iffy"));
    REQUIRE(matches_exactly<g::token::identifier>("letter"));
    REQUIRE(matches_exactly<g::token::identifier>("internal"));
    REQUIRE(matches_exactly<g::token::identifier>("within"));
  }

  SECTION("identifiers cannot start with digits") {
    REQUIRE_FALSE(matches_exactly<g::token::identifier>("1foo"));
    REQUIRE_FALSE(matches_exactly<g::token::identifier>("123"));
  }
}

// =============================================================================
// keyword tests
// =============================================================================

TEST_CASE("keyword parsing", "[grammar][token][keyword]") {
  REQUIRE(matches_exactly<g::token::keyword_if>("if"));
  REQUIRE(matches_exactly<g::token::keyword_then>("then"));
  REQUIRE(matches_exactly<g::token::keyword_else>("else"));
  REQUIRE(matches_exactly<g::token::keyword_let>("let"));
  REQUIRE(matches_exactly<g::token::keyword_in>("in"));
  REQUIRE(matches_exactly<g::token::keyword_with>("with"));
  REQUIRE(matches_exactly<g::token::keyword_assert>("assert"));
  REQUIRE(matches_exactly<g::token::keyword_rec>("rec"));
  REQUIRE(matches_exactly<g::token::keyword_inherit>("inherit"));
  REQUIRE(matches_exactly<g::token::keyword_or>("or"));

  SECTION("keywords don't match prefixes") {
    REQUIRE_FALSE(matches_exactly<g::token::keyword_if>("iffy"));
    REQUIRE_FALSE(matches_exactly<g::token::keyword_let>("letter"));
  }
}

// =============================================================================
// string literal tests
// =============================================================================

TEST_CASE("double-quoted string parsing", "[grammar][string]") {
  SECTION("empty string") {
    REQUIRE(matches_exactly<g::string_double_quoted>("\"\""));
  }

  SECTION("simple strings") {
    REQUIRE(matches_exactly<g::string_double_quoted>("\"hello\""));
    REQUIRE(matches_exactly<g::string_double_quoted>("\"hello world\""));
  }

  SECTION("strings with escapes") {
    REQUIRE(matches_exactly<g::string_double_quoted>("\"hello\\nworld\""));
    REQUIRE(matches_exactly<g::string_double_quoted>("\"tab\\there\""));
    REQUIRE(matches_exactly<g::string_double_quoted>("\"quote\\\"here\""));
    REQUIRE(matches_exactly<g::string_double_quoted>("\"backslash\\\\here\""));
  }

  SECTION("strings with interpolation") {
    REQUIRE(matches_exactly<g::string_double_quoted>("\"hello ${name}\""));
    REQUIRE(matches_exactly<g::string_double_quoted>("\"${a} and ${b}\""));
    REQUIRE(matches_exactly<g::string_double_quoted>("\"${1 + 2}\""));
  }

  SECTION("dollar sign without interpolation") {
    REQUIRE(matches_exactly<g::string_double_quoted>("\"$100\""));
    REQUIRE(matches_exactly<g::string_double_quoted>("\"$$\""));
  }

  SECTION("unclosed string fails") {
    REQUIRE_FALSE(matches_exactly<g::string_double_quoted>("\"unclosed"));
  }
}

TEST_CASE("indented string parsing", "[grammar][string]") {
  SECTION("empty indented string") {
    REQUIRE(matches_exactly<g::string_indented>("''''"));
  }

  SECTION("simple indented string") {
    REQUIRE(matches_exactly<g::string_indented>("''hello''"));
  }

  SECTION("multiline indented string") {
    REQUIRE(matches_exactly<g::string_indented>("''\n  line1\n  line2\n''"));
  }

  SECTION("indented string with interpolation") {
    REQUIRE(matches_exactly<g::string_indented>("''hello ${name}''"));
  }

  SECTION("indented string escapes") {
    REQUIRE(matches_exactly<g::string_indented>("''it''\\'''"));     // escape quote
    REQUIRE(matches_exactly<g::string_indented>("''dollar ''${''")); // escape interpolation
  }
}

// =============================================================================
// path tests
// =============================================================================

TEST_CASE("path parsing", "[grammar][path]") {
  SECTION("relative paths") {
    REQUIRE(matches_exactly<g::path>("./foo"));
    REQUIRE(matches_exactly<g::path>("./foo/bar"));
    REQUIRE(matches_exactly<g::path>("../parent"));
  }

  SECTION("absolute paths") {
    REQUIRE(matches_exactly<g::path>("/nix/store"));
    REQUIRE(matches_exactly<g::path>("/etc/nixos/configuration.nix"));
  }

  SECTION("home paths") {
    REQUIRE(matches_exactly<g::path>("~/config"));
    REQUIRE(matches_exactly<g::path>("~/.config/nix"));
  }

  SECTION("search paths") {
    REQUIRE(matches_exactly<g::path>("<nixpkgs>"));
    REQUIRE(matches_exactly<g::path>("<nixpkgs/lib>"));
  }

  SECTION("paths with interpolation") {
    REQUIRE(matches_exactly<g::path>("./foo/${name}"));
    REQUIRE(matches_exactly<g::path>("./${dir}/file"));
  }
}

// =============================================================================
// attribute path tests
// =============================================================================

TEST_CASE("attribute path parsing", "[grammar][attrpath]") {
  SECTION("simple attribute") {
    REQUIRE(matches_exactly<g::attribute_path>("foo"));
  }

  SECTION("dotted attribute path") {
    REQUIRE(matches_exactly<g::attribute_path>("foo.bar"));
    REQUIRE(matches_exactly<g::attribute_path>("a.b.c.d"));
  }

  SECTION("string attributes") {
    REQUIRE(matches_exactly<g::attribute_path>("\"foo\""));
    REQUIRE(matches_exactly<g::attribute_path>("foo.\"bar with spaces\""));
  }

  SECTION("dynamic attributes") {
    REQUIRE(matches_exactly<g::attribute_path>("${name}"));
    REQUIRE(matches_exactly<g::attribute_path>("foo.${bar}"));
  }

  SECTION("or keyword as attribute") {
    REQUIRE(matches_exactly<g::attribute_path>("or"));
    REQUIRE(matches_exactly<g::attribute_path>("foo.or"));
  }
}

// =============================================================================
// formals (lambda parameters) tests
// =============================================================================

TEST_CASE("formals parsing", "[grammar][formals]") {
  SECTION("empty formals") {
    REQUIRE(matches_exactly<g::formals>("{ }"));
  }

  SECTION("single formal") {
    REQUIRE(matches_exactly<g::formals>("{ x }"));
  }

  SECTION("multiple formals") {
    REQUIRE(matches_exactly<g::formals>("{ a, b, c }"));
  }

  SECTION("formals with defaults") {
    REQUIRE(matches_exactly<g::formals>("{ x ? 1 }"));
    REQUIRE(matches_exactly<g::formals>("{ a ? 1, b ? 2 }"));
  }

  SECTION("formals with ellipsis") {
    REQUIRE(matches_exactly<g::formals>("{ ... }"));
    REQUIRE(matches_exactly<g::formals>("{ x, ... }"));
    REQUIRE(matches_exactly<g::formals>("{ x, y, ... }"));
  }

  SECTION("trailing comma") {
    REQUIRE(matches_exactly<g::formals>("{ x, }"));
    REQUIRE(matches_exactly<g::formals>("{ x, y, }"));
  }
}

// =============================================================================
// expression tests
// =============================================================================

TEST_CASE("simple expression parsing", "[grammar][expr]") {
  SECTION("literals") {
    REQUIRE(matches_exactly<g::expression>("42"));
    REQUIRE(matches_exactly<g::expression>("3.14"));
    REQUIRE(matches_exactly<g::expression>("\"hello\""));
    REQUIRE(matches_exactly<g::expression>("./path"));
  }

  SECTION("identifiers") {
    REQUIRE(matches_exactly<g::expression>("foo"));
    REQUIRE(matches_exactly<g::expression>("bar_baz"));
  }

  SECTION("parenthesized expressions") {
    REQUIRE(matches_exactly<g::expression>("(42)"));
    REQUIRE(matches_exactly<g::expression>("((1))"));
  }
}

TEST_CASE("list expression parsing", "[grammar][expr]") {
  SECTION("empty list") {
    REQUIRE(matches_exactly<g::expression>("[ ]"));
  }

  SECTION("simple list") {
    REQUIRE(matches_exactly<g::expression>("[ 1 2 3 ]"));
  }

  SECTION("list with various elements") {
    REQUIRE(matches_exactly<g::expression>("[ 1 \"hello\" ./path ]"));
  }

  SECTION("nested lists") {
    REQUIRE(matches_exactly<g::expression>("[ [ 1 2 ] [ 3 4 ] ]"));
  }
}

TEST_CASE("attribute set expression parsing", "[grammar][expr]") {
  SECTION("empty set") {
    REQUIRE(matches_exactly<g::expression>("{ }"));
  }

  SECTION("simple set") {
    REQUIRE(matches_exactly<g::expression>("{ x = 1; }"));
  }

  SECTION("multiple bindings") {
    REQUIRE(matches_exactly<g::expression>("{ a = 1; b = 2; }"));
  }

  SECTION("nested paths") {
    REQUIRE(matches_exactly<g::expression>("{ a.b.c = 1; }"));
  }

  SECTION("recursive set") {
    REQUIRE(matches_exactly<g::expression>("rec { x = 1; y = x; }"));
  }

  SECTION("inherit") {
    REQUIRE(matches_exactly<g::expression>("{ inherit x; }"));
    REQUIRE(matches_exactly<g::expression>("{ inherit x y z; }"));
    REQUIRE(matches_exactly<g::expression>("{ inherit (pkg) lib; }"));
  }
}

TEST_CASE("binary operator expression parsing", "[grammar][expr]") {
  SECTION("arithmetic operators") {
    REQUIRE(matches_exactly<g::expression>("1 + 2"));
    REQUIRE(matches_exactly<g::expression>("3 - 1"));
    REQUIRE(matches_exactly<g::expression>("2 * 3"));
    REQUIRE(matches_exactly<g::expression>("6 / 2"));
  }

  SECTION("comparison operators") {
    REQUIRE(matches_exactly<g::expression>("1 < 2"));
    REQUIRE(matches_exactly<g::expression>("2 > 1"));
    REQUIRE(matches_exactly<g::expression>("1 <= 1"));
    REQUIRE(matches_exactly<g::expression>("2 >= 2"));
    REQUIRE(matches_exactly<g::expression>("1 == 1"));
    REQUIRE(matches_exactly<g::expression>("1 != 2"));
  }

  SECTION("logical operators") {
    REQUIRE(matches_exactly<g::expression>("true && false"));
    REQUIRE(matches_exactly<g::expression>("true || false"));
    REQUIRE(matches_exactly<g::expression>("true -> false"));
  }

  SECTION("list operators") {
    REQUIRE(matches_exactly<g::expression>("[ 1 ] ++ [ 2 ]"));
  }

  SECTION("attrset operators") {
    REQUIRE(matches_exactly<g::expression>("{ } // { x = 1; }"));
  }

  SECTION("pipe operators") {
    REQUIRE(matches_exactly<g::expression>("x |> f"));
    REQUIRE(matches_exactly<g::expression>("f <| x"));
  }
}

TEST_CASE("unary operator expression parsing", "[grammar][expr]") {
  SECTION("logical not") {
    REQUIRE(matches_exactly<g::expression>("!true"));
    REQUIRE(matches_exactly<g::expression>("! false"));
  }

  SECTION("unary minus") {
    REQUIRE(matches_exactly<g::expression>("-1"));
    REQUIRE(matches_exactly<g::expression>("- 42"));
  }
}

TEST_CASE("function application parsing", "[grammar][expr]") {
  SECTION("single argument") {
    REQUIRE(matches_exactly<g::expression>("f x"));
  }

  SECTION("multiple arguments") {
    REQUIRE(matches_exactly<g::expression>("f x y z"));
  }

  SECTION("application with literals") {
    REQUIRE(matches_exactly<g::expression>("f 1 2 3"));
  }

  SECTION("chained application") {
    REQUIRE(matches_exactly<g::expression>("(f x) y"));
  }
}

TEST_CASE("attribute selection parsing", "[grammar][expr]") {
  SECTION("simple selection") {
    REQUIRE(matches_exactly<g::expression>("x.y"));
    REQUIRE(matches_exactly<g::expression>("a.b.c"));
  }

  SECTION("selection with or default") {
    REQUIRE(matches_exactly<g::expression>("x.y or 0"));
    REQUIRE(matches_exactly<g::expression>("a.b.c or null"));
  }

  SECTION("has attribute") {
    REQUIRE(matches_exactly<g::expression>("x ? y"));
    REQUIRE(matches_exactly<g::expression>("a ? b.c"));
  }
}

TEST_CASE("lambda expression parsing", "[grammar][expr]") {
  SECTION("simple lambda") {
    REQUIRE(matches_exactly<g::expression>("x: x"));
    REQUIRE(matches_exactly<g::expression>("x: x + 1"));
  }

  SECTION("pattern lambda") {
    REQUIRE(matches_exactly<g::expression>("{ x }: x"));
    REQUIRE(matches_exactly<g::expression>("{ x, y }: x + y"));
    REQUIRE(matches_exactly<g::expression>("{ x ? 1 }: x"));
    REQUIRE(matches_exactly<g::expression>("{ ... }: 1"));
  }

  SECTION("pattern with @ binding") {
    REQUIRE(matches_exactly<g::expression>("{ x }@args: args"));
    REQUIRE(matches_exactly<g::expression>("args@{ x }: x"));
  }
}

TEST_CASE("let expression parsing", "[grammar][expr]") {
  SECTION("simple let") {
    REQUIRE(matches_exactly<g::expression>("let x = 1; in x"));
  }

  SECTION("multiple bindings") {
    REQUIRE(matches_exactly<g::expression>("let a = 1; b = 2; in a + b"));
  }

  SECTION("let with inherit") {
    REQUIRE(matches_exactly<g::expression>("let inherit (x) y; in y"));
  }
}

TEST_CASE("if expression parsing", "[grammar][expr]") {
  SECTION("simple if") {
    REQUIRE(matches_exactly<g::expression>("if true then 1 else 2"));
  }

  SECTION("nested if") {
    REQUIRE(matches_exactly<g::expression>("if a then if b then 1 else 2 else 3"));
  }
}

TEST_CASE("with expression parsing", "[grammar][expr]") {
  REQUIRE(matches_exactly<g::expression>("with pkgs; hello"));
  REQUIRE(matches_exactly<g::expression>("with lib; with builtins; x"));
}

TEST_CASE("assert expression parsing", "[grammar][expr]") {
  REQUIRE(matches_exactly<g::expression>("assert true; 1"));
  REQUIRE(matches_exactly<g::expression>("assert x != null; x"));
}

// =============================================================================
// complex expression tests
// =============================================================================

TEST_CASE("operator precedence in grammar", "[grammar][precedence]") {
  // these should all parse (precedence handled by actions, not grammar)
  REQUIRE(matches_exactly<g::expression>("1 + 2 * 3"));
  REQUIRE(matches_exactly<g::expression>("1 * 2 + 3"));
  REQUIRE(matches_exactly<g::expression>("1 < 2 && 3 > 0"));
  REQUIRE(matches_exactly<g::expression>("!a && b || c"));
}

TEST_CASE("real-world expression patterns", "[grammar][real]") {
  // These use g::root because the raw strings have leading/trailing whitespace
  SECTION("derivation-like") {
    REQUIRE(matches_exactly<g::root>(R"(
      { stdenv, fetchurl }:
      stdenv.mkDerivation {
        name = "hello";
        src = fetchurl {
          url = "http://example.com/hello.tar.gz";
        };
      }
    )"));
  }

  SECTION("nixos module-like") {
    REQUIRE(matches_exactly<g::root>(R"(
      { config, lib, pkgs, ... }:
      with lib;
      {
        options.services.foo.enable = mkOption {
          type = types.bool;
          default = false;
        };
      }
    )"));
  }

  SECTION("flake-like") {
    REQUIRE(matches_exactly<g::root>(R"(
      {
        inputs.nixpkgs.url = "github:NixOS/nixpkgs";
        outputs = { self, nixpkgs }: {
          packages.x86_64-linux.default = nixpkgs.hello;
        };
      }
    )"));
  }
}

// =============================================================================
// whitespace and comments
// =============================================================================

TEST_CASE("whitespace handling", "[grammar][whitespace]") {
  SECTION("spaces between tokens") {
    REQUIRE(matches_exactly<g::expression>("1    +    2"));
  }

  SECTION("newlines between tokens") {
    REQUIRE(matches_exactly<g::expression>("1\n+\n2"));
  }

  SECTION("tabs between tokens") {
    REQUIRE(matches_exactly<g::expression>("1\t+\t2"));
  }
}

TEST_CASE("comment handling", "[grammar][comment]") {
  SECTION("line comments") {
    REQUIRE(matches_exactly<g::expression>("1 # comment\n+ 2"));
  }

  SECTION("block comments between tokens") {
    REQUIRE(matches_exactly<g::expression>("1 /* block */ + 2"));
  }

  SECTION("block comments at boundaries") {
    // Leading/trailing whitespace is handled by the root rule, not expression
    REQUIRE(matches_exactly<g::root>("/* start */ 1 + 2 /* end */"));
  }

  SECTION("non-nested block comments") {
    // Nix doesn't support nested comments - /* outer /* not nested */ ends at first */
    // After that, */ is not valid syntax, so this shouldn't parse as an expression
    // The remaining "* / 1" or "*/1" depends on tokenization
    REQUIRE_FALSE(matches_exactly<g::expression>("/* outer /* not nested */ */ 1"));
    // But the comment content itself is valid
    REQUIRE(matches_exactly<g::root>("/* outer /* inner */ 1"));
  }
}

// =============================================================================
// error cases
// =============================================================================

TEST_CASE("grammar rejects invalid input", "[grammar][error]") {
  SECTION("unclosed structures") {
    REQUIRE_FALSE(matches_exactly<g::expression>("["));
    REQUIRE_FALSE(matches_exactly<g::expression>("{ x = 1"));
    REQUIRE_FALSE(matches_exactly<g::expression>("\"unclosed"));
  }

  SECTION("missing parts") {
    REQUIRE_FALSE(matches_exactly<g::expression>("if then else"));
    REQUIRE_FALSE(matches_exactly<g::expression>("let in"));
  }

  SECTION("invalid tokens") {
    REQUIRE_FALSE(matches_exactly<g::expression>("@invalid"));
  }
}
