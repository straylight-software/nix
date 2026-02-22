// straylight // nix // store // tests
//
// Schema compatibility tests - executable specification for Nix database compatibility
//
// These tests verify that straylight-nix uses the same database schema as upstream nix.
// The column names MUST match exactly for compatibility with existing nix installations.
//
// Background:
//   - Nix stores metadata in SQLite databases (/nix/var/nix/db/db.sqlite, binary cache)
//   - Column names must match existing databases or queries will fail
//   - A snake_case refactoring broke compatibility - these tests prevent regression

#include <fstream>
#include <regex>
#include <sstream>
#include <string>
#include <string_view>

#include <catch2/catch_test_macros.hpp>

namespace {

// =============================================================================
// Test helpers
// =============================================================================

std::string read_file(const std::string& path) {
  std::ifstream file(path);
  if (!file.is_open()) {
    return "";
  }
  std::stringstream buffer;
  buffer << file.rdbuf();
  return buffer.str();
}

bool schema_contains_column(const std::string& schema, const std::string& column_name) {
  // Match column name as a whole word (not as part of another word)
  // This handles both "column_name" and "column_name type" patterns
  std::string pattern = std::string("\\b") + column_name + "\\b";
  std::regex re(pattern);
  return std::regex_search(schema, re);
}

} // namespace

// =============================================================================
// Store database schema tests (schema.sql.gen.h)
// =============================================================================

TEST_CASE("ValidPaths schema uses camelCase column names", "[store][schema][compatibility]") {
  // Read the schema file directly
  std::string schema = read_file("src/nix/store/schema.sql.gen.h");
  REQUIRE_FALSE(schema.empty());

  SECTION("narSize column is camelCase (NOT nar_size)") {
    INFO("Schema must use 'narSize' for compatibility with existing nix databases");
    REQUIRE(schema_contains_column(schema, "narSize"));
    REQUIRE_FALSE(schema_contains_column(schema, "nar_size"));
  }

  SECTION("registrationTime column is camelCase") {
    INFO("Schema must use 'registrationTime' for compatibility");
    REQUIRE(schema_contains_column(schema, "registrationTime"));
    REQUIRE_FALSE(schema_contains_column(schema, "registration_time"));
  }
}

// =============================================================================
// CA-derivations schema tests (ca-specific-schema.sql.gen.h)
// =============================================================================

TEST_CASE("Realisations schema uses camelCase column names", "[store][schema][compatibility]") {
  std::string schema = read_file("src/nix/store/ca-specific-schema.sql.gen.h");
  REQUIRE_FALSE(schema.empty());

  SECTION("drvPath column is camelCase (NOT drv_path)") {
    INFO("Schema must use 'drvPath' for compatibility with existing nix databases");
    REQUIRE(schema_contains_column(schema, "drvPath"));
    REQUIRE_FALSE(schema_contains_column(schema, "drv_path"));
  }

  SECTION("outputName column is camelCase (NOT output_name)") {
    INFO("Schema must use 'outputName' for compatibility");
    REQUIRE(schema_contains_column(schema, "outputName"));
    REQUIRE_FALSE(schema_contains_column(schema, "output_name"));
  }

  SECTION("outputPath column is camelCase (NOT output_path)") {
    INFO("Schema must use 'outputPath' for compatibility");
    REQUIRE(schema_contains_column(schema, "outputPath"));
    REQUIRE_FALSE(schema_contains_column(schema, "output_path"));
  }
}

// =============================================================================
// Binary cache schema tests (nar-info-disk-cache.cpp)
// =============================================================================

// Extract just the SQL schema from the nar-info-disk-cache.cpp file
// The schema is defined between R"sql( and )sql";
std::string extract_sql_schema(const std::string& content) {
  auto start = content.find("R\"sql(");
  if (start == std::string::npos)
    return "";
  auto end = content.find(")sql\";", start);
  if (end == std::string::npos)
    return "";
  return content.substr(start, end - start + 6);
}

TEST_CASE("Binary cache schema uses camelCase column names", "[store][schema][compatibility]") {
  std::string file_content = read_file("src/nix/store/nar-info-disk-cache.cpp");
  REQUIRE_FALSE(file_content.empty());

  // Extract just the SQL schema, not C++ code
  std::string schema = extract_sql_schema(file_content);
  REQUIRE_FALSE(schema.empty());

  SECTION("BinaryCaches table uses camelCase") {
    INFO("BinaryCaches must use 'storeDir' (NOT store_dir)");
    REQUIRE(schema_contains_column(schema, "storeDir"));
    REQUIRE_FALSE(schema_contains_column(schema, "store_dir"));

    INFO("BinaryCaches must use 'wantMassQuery' (NOT want_mass_query)");
    REQUIRE(schema_contains_column(schema, "wantMassQuery"));
    REQUIRE_FALSE(schema_contains_column(schema, "want_mass_query"));
  }

  SECTION("NARs table uses camelCase") {
    INFO("NARs must use 'hashPart' (NOT hash_part)");
    REQUIRE(schema_contains_column(schema, "hashPart"));
    REQUIRE_FALSE(schema_contains_column(schema, "hash_part"));

    INFO("NARs must use 'namePart' (NOT name_part)");
    REQUIRE(schema_contains_column(schema, "namePart"));
    REQUIRE_FALSE(schema_contains_column(schema, "name_part"));

    INFO("NARs must use 'fileSize' (NOT file_size)");
    REQUIRE(schema_contains_column(schema, "fileSize"));
    REQUIRE_FALSE(schema_contains_column(schema, "file_size"));

    INFO("NARs must use 'narHash' (NOT nar_hash)");
    REQUIRE(schema_contains_column(schema, "narHash"));
    REQUIRE_FALSE(schema_contains_column(schema, "nar_hash"));
  }

  SECTION("Cache Realisations table uses camelCase") {
    INFO("Realisations must use 'outputId' (NOT output_id)");
    REQUIRE(schema_contains_column(schema, "outputId"));
    REQUIRE_FALSE(schema_contains_column(schema, "output_id"));
  }
}

// =============================================================================
// SQL query compatibility tests (local-store.cpp)
// =============================================================================

TEST_CASE("SQL queries in local-store use camelCase column names",
          "[store][schema][compatibility]") {
  std::string code = read_file("src/nix/store/local-store.cpp");
  REQUIRE_FALSE(code.empty());

  SECTION("Realisations queries use camelCase") {
    // The SQL queries should use drvPath, outputName, outputPath
    // Check that queries don't use snake_case

    // Find the RegisterRealisedOutput query and check it uses camelCase
    // The query inserts into: drvPath, outputName, outputPath, signatures
    INFO("insert into Realisations must use camelCase column names");

    // We check for the presence of the correct column names in queries
    // Note: This is a heuristic check - the actual queries are in string literals
    std::regex insert_query(R"(insert\s+into\s+Realisations\s*\([^)]*drvPath)");
    REQUIRE(std::regex_search(code, insert_query));

    // Check for absence of snake_case in queries
    std::regex snake_case_insert(R"(insert\s+into\s+Realisations\s*\([^)]*drv_path)");
    REQUIRE_FALSE(std::regex_search(code, snake_case_insert));
  }
}
