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

bool schema_contains_index(const std::string& schema, const std::string& index_name) {
  // Match index name in CREATE INDEX statements
  std::string pattern = std::string("create index if not exists ") + index_name + "\\b";
  std::regex re(pattern, std::regex_constants::icase);
  return std::regex_search(schema, re);
}

bool schema_contains_table(const std::string& schema, const std::string& table_name) {
  std::string pattern = std::string("create table if not exists ") + table_name + "\\b";
  std::regex re(pattern, std::regex_constants::icase);
  return std::regex_search(schema, re);
}

bool schema_contains_foreign_key(const std::string& schema, const std::string& column,
                                 const std::string& ref_table, const std::string& ref_column) {
  // Match: foreign key (column) references ref_table(ref_column)
  std::string pattern = std::string("foreign key\\s*\\(") + column + "\\)\\s*references\\s+" +
                        ref_table + "\\s*\\(" + ref_column + "\\)";
  std::regex re(pattern, std::regex_constants::icase);
  return std::regex_search(schema, re);
}

bool schema_contains_trigger(const std::string& schema, const std::string& trigger_name) {
  std::string pattern = std::string("create trigger if not exists ") + trigger_name + "\\b";
  std::regex re(pattern, std::regex_constants::icase);
  return std::regex_search(schema, re);
}

// Extract just the SQL schema from a file containing R"sql( ... )sql";
std::string extract_sql_schema(const std::string& content) {
  auto start = content.find("R\"sql(");
  if (start == std::string::npos)
    return "";
  auto end = content.find(")sql\"", start);
  if (end == std::string::npos)
    return "";
  return content.substr(start, end - start + 5);
}

} // namespace

// =============================================================================
// Store database schema tests (schema.sql.gen.h)
// =============================================================================

TEST_CASE("ValidPaths table schema", "[store][schema][compatibility]") {
  std::string schema = read_file("src/nix/store/schema.sql.gen.h");
  REQUIRE_FALSE(schema.empty());

  SECTION("ValidPaths table exists") {
    REQUIRE(schema_contains_table(schema, "ValidPaths"));
  }

  SECTION("id column - primary key") {
    INFO("ValidPaths must have 'id' as integer primary key");
    REQUIRE(schema_contains_column(schema, "id"));
  }

  SECTION("path column - store path string") {
    INFO("ValidPaths must have 'path' column for store path");
    REQUIRE(schema_contains_column(schema, "path"));
  }

  SECTION("hash column - base16 representation") {
    INFO("ValidPaths must have 'hash' column for content hash");
    REQUIRE(schema_contains_column(schema, "hash"));
  }

  SECTION("registrationTime column is camelCase") {
    INFO("Schema must use 'registrationTime' for compatibility");
    REQUIRE(schema_contains_column(schema, "registrationTime"));
    REQUIRE_FALSE(schema_contains_column(schema, "registration_time"));
  }

  SECTION("deriver column - optional derivation path") {
    INFO("ValidPaths must have 'deriver' column");
    REQUIRE(schema_contains_column(schema, "deriver"));
  }

  SECTION("narSize column is camelCase (NOT nar_size)") {
    INFO("Schema must use 'narSize' for compatibility with existing nix databases");
    REQUIRE(schema_contains_column(schema, "narSize"));
    REQUIRE_FALSE(schema_contains_column(schema, "nar_size"));
  }

  SECTION("ultimate column - boolean flag") {
    INFO("ValidPaths must have 'ultimate' column");
    REQUIRE(schema_contains_column(schema, "ultimate"));
  }

  SECTION("sigs column - space-separated signatures") {
    INFO("ValidPaths must have 'sigs' column");
    REQUIRE(schema_contains_column(schema, "sigs"));
  }

  SECTION("ca column - content address assertion") {
    INFO("ValidPaths must have 'ca' column for content-addressed paths");
    REQUIRE(schema_contains_column(schema, "ca"));
  }
}

TEST_CASE("Refs table schema", "[store][schema][compatibility]") {
  std::string schema = read_file("src/nix/store/schema.sql.gen.h");
  REQUIRE_FALSE(schema.empty());

  SECTION("Refs table exists") {
    REQUIRE(schema_contains_table(schema, "Refs"));
  }

  SECTION("referrer column") {
    INFO("Refs must have 'referrer' column");
    REQUIRE(schema_contains_column(schema, "referrer"));
    REQUIRE_FALSE(schema_contains_column(schema, "referrer_id"));
  }

  SECTION("reference column") {
    INFO("Refs must have 'reference' column");
    REQUIRE(schema_contains_column(schema, "reference"));
    REQUIRE_FALSE(schema_contains_column(schema, "reference_id"));
  }

  SECTION("foreign key: referrer -> ValidPaths(id)") {
    INFO("Refs.referrer must reference ValidPaths.id");
    REQUIRE(schema_contains_foreign_key(schema, "referrer", "ValidPaths", "id"));
  }

  SECTION("foreign key: reference -> ValidPaths(id)") {
    INFO("Refs.reference must reference ValidPaths.id");
    REQUIRE(schema_contains_foreign_key(schema, "reference", "ValidPaths", "id"));
  }
}

TEST_CASE("DerivationOutputs table schema", "[store][schema][compatibility]") {
  std::string schema = read_file("src/nix/store/schema.sql.gen.h");
  REQUIRE_FALSE(schema.empty());

  SECTION("DerivationOutputs table exists") {
    REQUIRE(schema_contains_table(schema, "DerivationOutputs"));
  }

  SECTION("drv column - derivation id") {
    INFO("DerivationOutputs must have 'drv' column");
    REQUIRE(schema_contains_column(schema, "drv"));
  }

  SECTION("id column - symbolic output id") {
    INFO("DerivationOutputs must have 'id' column for output name");
    // Note: 'id' appears in both ValidPaths and DerivationOutputs but with different meanings
    REQUIRE(schema_contains_column(schema, "id"));
  }

  SECTION("path column - output path") {
    INFO("DerivationOutputs must have 'path' column");
    REQUIRE(schema_contains_column(schema, "path"));
  }

  SECTION("foreign key: drv -> ValidPaths(id)") {
    INFO("DerivationOutputs.drv must reference ValidPaths.id");
    REQUIRE(schema_contains_foreign_key(schema, "drv", "ValidPaths", "id"));
  }
}

TEST_CASE("Store schema index names match upstream", "[store][schema][compatibility]") {
  std::string schema = read_file("src/nix/store/schema.sql.gen.h");
  REQUIRE_FALSE(schema.empty());

  SECTION("IndexReferrer exists") {
    INFO("Index on Refs(referrer) must be named 'IndexReferrer'");
    REQUIRE(schema_contains_index(schema, "IndexReferrer"));
  }

  SECTION("IndexReference exists") {
    INFO("Index on Refs(reference) must be named 'IndexReference'");
    REQUIRE(schema_contains_index(schema, "IndexReference"));
  }

  SECTION("IndexDerivationOutputs exists") {
    INFO("Index on DerivationOutputs(path) must be named 'IndexDerivationOutputs'");
    REQUIRE(schema_contains_index(schema, "IndexDerivationOutputs"));
  }
}

TEST_CASE("Store schema triggers", "[store][schema][compatibility]") {
  std::string schema = read_file("src/nix/store/schema.sql.gen.h");
  REQUIRE_FALSE(schema.empty());

  SECTION("DeleteSelfRefs trigger exists") {
    INFO("Trigger to delete self-references must exist");
    REQUIRE(schema_contains_trigger(schema, "DeleteSelfRefs"));
  }
}

// =============================================================================
// CA-derivations schema tests (ca-specific-schema.sql.gen.h)
// =============================================================================

TEST_CASE("Realisations table schema", "[store][schema][compatibility]") {
  std::string schema = read_file("src/nix/store/ca-specific-schema.sql.gen.h");
  REQUIRE_FALSE(schema.empty());

  SECTION("Realisations table exists") {
    REQUIRE(schema_contains_table(schema, "Realisations"));
  }

  SECTION("id column - primary key") {
    INFO("Realisations must have 'id' as primary key");
    REQUIRE(schema_contains_column(schema, "id"));
  }

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

  SECTION("signatures column") {
    INFO("Realisations must have 'signatures' column");
    REQUIRE(schema_contains_column(schema, "signatures"));
  }

  SECTION("foreign key: outputPath -> ValidPaths(id)") {
    INFO("Realisations.outputPath must reference ValidPaths.id");
    REQUIRE(schema_contains_foreign_key(schema, "outputPath", "ValidPaths", "id"));
  }
}

TEST_CASE("RealisationsRefs table schema", "[store][schema][compatibility]") {
  std::string schema = read_file("src/nix/store/ca-specific-schema.sql.gen.h");
  REQUIRE_FALSE(schema.empty());

  SECTION("RealisationsRefs table exists") {
    REQUIRE(schema_contains_table(schema, "RealisationsRefs"));
  }

  SECTION("referrer column") {
    INFO("RealisationsRefs must have 'referrer' column");
    REQUIRE(schema_contains_column(schema, "referrer"));
  }

  SECTION("realisationReference column is camelCase") {
    INFO("RealisationsRefs must use 'realisationReference' column");
    REQUIRE(schema_contains_column(schema, "realisationReference"));
    REQUIRE_FALSE(schema_contains_column(schema, "realisation_reference"));
  }

  SECTION("foreign key: referrer -> Realisations(id)") {
    INFO("RealisationsRefs.referrer must reference Realisations.id");
    REQUIRE(schema_contains_foreign_key(schema, "referrer", "Realisations", "id"));
  }

  SECTION("foreign key: realisationReference -> Realisations(id)") {
    INFO("RealisationsRefs.realisationReference must reference Realisations.id");
    REQUIRE(schema_contains_foreign_key(schema, "realisationReference", "Realisations", "id"));
  }
}

TEST_CASE("CA schema index names match upstream", "[store][schema][compatibility]") {
  std::string schema = read_file("src/nix/store/ca-specific-schema.sql.gen.h");
  REQUIRE_FALSE(schema.empty());

  SECTION("IndexRealisations exists") {
    INFO("Index on Realisations(drvPath, outputName) must be named 'IndexRealisations'");
    REQUIRE(schema_contains_index(schema, "IndexRealisations"));
  }

  SECTION("IndexRealisationsRefsRealisationReference exists") {
    INFO("Index must be named 'IndexRealisationsRefsRealisationReference'");
    REQUIRE(schema_contains_index(schema, "IndexRealisationsRefsRealisationReference"));
  }

  SECTION("IndexRealisationsRefs exists") {
    INFO("Index on RealisationsRefs(referrer) must be named 'IndexRealisationsRefs'");
    REQUIRE(schema_contains_index(schema, "IndexRealisationsRefs"));
  }

  SECTION("IndexRealisationsRefsOnOutputPath exists") {
    INFO("Index must be named 'IndexRealisationsRefsOnOutputPath'");
    REQUIRE(schema_contains_index(schema, "IndexRealisationsRefsOnOutputPath"));
  }
}

TEST_CASE("CA schema triggers", "[store][schema][compatibility]") {
  std::string schema = read_file("src/nix/store/ca-specific-schema.sql.gen.h");
  REQUIRE_FALSE(schema.empty());

  SECTION("DeleteSelfRefsViaRealisations trigger exists") {
    INFO("Trigger to handle CA self-references must exist");
    REQUIRE(schema_contains_trigger(schema, "DeleteSelfRefsViaRealisations"));
  }
}

// =============================================================================
// Binary cache schema tests (nar-info-disk-cache.cpp)
// =============================================================================

TEST_CASE("BinaryCaches table schema", "[store][schema][compatibility]") {
  std::string file_content = read_file("src/nix/store/nar-info-disk-cache.cpp");
  REQUIRE_FALSE(file_content.empty());
  std::string schema = extract_sql_schema(file_content);
  REQUIRE_FALSE(schema.empty());

  SECTION("BinaryCaches table exists") {
    REQUIRE(schema_contains_table(schema, "BinaryCaches"));
  }

  SECTION("id column") {
    REQUIRE(schema_contains_column(schema, "id"));
  }

  SECTION("url column") {
    REQUIRE(schema_contains_column(schema, "url"));
  }

  SECTION("timestamp column") {
    REQUIRE(schema_contains_column(schema, "timestamp"));
  }

  SECTION("storeDir column is camelCase (NOT store_dir)") {
    INFO("BinaryCaches must use 'storeDir' (NOT store_dir)");
    REQUIRE(schema_contains_column(schema, "storeDir"));
    REQUIRE_FALSE(schema_contains_column(schema, "store_dir"));
  }

  SECTION("wantMassQuery column is camelCase (NOT want_mass_query)") {
    INFO("BinaryCaches must use 'wantMassQuery' (NOT want_mass_query)");
    REQUIRE(schema_contains_column(schema, "wantMassQuery"));
    REQUIRE_FALSE(schema_contains_column(schema, "want_mass_query"));
  }

  SECTION("priority column") {
    REQUIRE(schema_contains_column(schema, "priority"));
  }
}

TEST_CASE("NARs table schema", "[store][schema][compatibility]") {
  std::string file_content = read_file("src/nix/store/nar-info-disk-cache.cpp");
  REQUIRE_FALSE(file_content.empty());
  std::string schema = extract_sql_schema(file_content);
  REQUIRE_FALSE(schema.empty());

  SECTION("NARs table exists") {
    REQUIRE(schema_contains_table(schema, "NARs"));
  }

  SECTION("cache column") {
    REQUIRE(schema_contains_column(schema, "cache"));
  }

  SECTION("hashPart column is camelCase (NOT hash_part)") {
    INFO("NARs must use 'hashPart' (NOT hash_part)");
    REQUIRE(schema_contains_column(schema, "hashPart"));
    REQUIRE_FALSE(schema_contains_column(schema, "hash_part"));
  }

  SECTION("namePart column is camelCase (NOT name_part)") {
    INFO("NARs must use 'namePart' (NOT name_part)");
    REQUIRE(schema_contains_column(schema, "namePart"));
    REQUIRE_FALSE(schema_contains_column(schema, "name_part"));
  }

  SECTION("url column") {
    REQUIRE(schema_contains_column(schema, "url"));
  }

  SECTION("compression column") {
    REQUIRE(schema_contains_column(schema, "compression"));
  }

  SECTION("fileHash column is camelCase (NOT file_hash)") {
    INFO("NARs must use 'fileHash' (NOT file_hash)");
    REQUIRE(schema_contains_column(schema, "fileHash"));
    REQUIRE_FALSE(schema_contains_column(schema, "file_hash"));
  }

  SECTION("fileSize column is camelCase (NOT file_size)") {
    INFO("NARs must use 'fileSize' (NOT file_size)");
    REQUIRE(schema_contains_column(schema, "fileSize"));
    REQUIRE_FALSE(schema_contains_column(schema, "file_size"));
  }

  SECTION("narHash column is camelCase (NOT nar_hash)") {
    INFO("NARs must use 'narHash' (NOT nar_hash)");
    REQUIRE(schema_contains_column(schema, "narHash"));
    REQUIRE_FALSE(schema_contains_column(schema, "nar_hash"));
  }

  SECTION("narSize column is camelCase (NOT nar_size)") {
    INFO("NARs must use 'narSize' (NOT nar_size)");
    REQUIRE(schema_contains_column(schema, "narSize"));
    REQUIRE_FALSE(schema_contains_column(schema, "nar_size"));
  }

  SECTION("refs column") {
    REQUIRE(schema_contains_column(schema, "refs"));
  }

  SECTION("deriver column") {
    REQUIRE(schema_contains_column(schema, "deriver"));
  }

  SECTION("sigs column") {
    REQUIRE(schema_contains_column(schema, "sigs"));
  }

  SECTION("ca column") {
    REQUIRE(schema_contains_column(schema, "ca"));
  }

  SECTION("timestamp column") {
    REQUIRE(schema_contains_column(schema, "timestamp"));
  }

  SECTION("present column") {
    REQUIRE(schema_contains_column(schema, "present"));
  }

  SECTION("foreign key: cache -> BinaryCaches(id)") {
    INFO("NARs.cache must reference BinaryCaches.id");
    REQUIRE(schema_contains_foreign_key(schema, "cache", "BinaryCaches", "id"));
  }
}

TEST_CASE("Cache Realisations table schema", "[store][schema][compatibility]") {
  std::string file_content = read_file("src/nix/store/nar-info-disk-cache.cpp");
  REQUIRE_FALSE(file_content.empty());
  std::string schema = extract_sql_schema(file_content);
  REQUIRE_FALSE(schema.empty());

  SECTION("Realisations table exists in cache schema") {
    REQUIRE(schema_contains_table(schema, "Realisations"));
  }

  SECTION("cache column") {
    REQUIRE(schema_contains_column(schema, "cache"));
  }

  SECTION("outputId column is camelCase (NOT output_id)") {
    INFO("Cache Realisations must use 'outputId' (NOT output_id)");
    REQUIRE(schema_contains_column(schema, "outputId"));
    REQUIRE_FALSE(schema_contains_column(schema, "output_id"));
  }

  SECTION("content column") {
    REQUIRE(schema_contains_column(schema, "content"));
  }

  SECTION("timestamp column") {
    REQUIRE(schema_contains_column(schema, "timestamp"));
  }

  SECTION("foreign key: cache -> BinaryCaches(id)") {
    INFO("Realisations.cache must reference BinaryCaches.id");
    REQUIRE(schema_contains_foreign_key(schema, "cache", "BinaryCaches", "id"));
  }
}

TEST_CASE("LastPurge table schema", "[store][schema][compatibility]") {
  std::string file_content = read_file("src/nix/store/nar-info-disk-cache.cpp");
  REQUIRE_FALSE(file_content.empty());
  std::string schema = extract_sql_schema(file_content);
  REQUIRE_FALSE(schema.empty());

  SECTION("LastPurge table exists") {
    REQUIRE(schema_contains_table(schema, "LastPurge"));
  }

  SECTION("dummy column") {
    REQUIRE(schema_contains_column(schema, "dummy"));
  }

  SECTION("value column") {
    REQUIRE(schema_contains_column(schema, "value"));
  }
}

// =============================================================================
// SQL query compatibility tests (local-store.cpp)
// =============================================================================

TEST_CASE("RegisterValidPath query uses correct columns", "[store][schema][compatibility]") {
  std::string code = read_file("src/nix/store/local-store.cpp");
  REQUIRE_FALSE(code.empty());

  SECTION("INSERT uses camelCase column names") {
    // The RegisterValidPath query should include these camelCase columns
    std::regex insert_query(
        R"(insert into ValidPaths\s*\([^)]*registrationTime[^)]*narSize[^)]*\))");
    REQUIRE(std::regex_search(code, insert_query));
  }

  SECTION("INSERT does not use snake_case") {
    std::regex snake_case(R"(insert into ValidPaths\s*\([^)]*registration_time)");
    REQUIRE_FALSE(std::regex_search(code, snake_case));

    std::regex snake_case2(R"(insert into ValidPaths\s*\([^)]*nar_size)");
    REQUIRE_FALSE(std::regex_search(code, snake_case2));
  }
}

TEST_CASE("UpdatePathInfo query uses correct columns", "[store][schema][compatibility]") {
  std::string code = read_file("src/nix/store/local-store.cpp");
  REQUIRE_FALSE(code.empty());

  SECTION("UPDATE uses camelCase column names") {
    std::regex update_query(R"(update ValidPaths set narSize)");
    REQUIRE(std::regex_search(code, update_query));
  }

  SECTION("UPDATE does not use snake_case") {
    std::regex snake_case(R"(update ValidPaths set nar_size)");
    REQUIRE_FALSE(std::regex_search(code, snake_case));
  }
}

TEST_CASE("QueryPathInfo query uses correct columns", "[store][schema][compatibility]") {
  std::string code = read_file("src/nix/store/local-store.cpp");
  REQUIRE_FALSE(code.empty());

  SECTION("SELECT uses camelCase column names") {
    std::regex select_query(R"(select id, hash, registrationTime, deriver, narSize)");
    REQUIRE(std::regex_search(code, select_query));
  }

  SECTION("SELECT does not use snake_case") {
    std::regex snake_case(R"(select[^;]*registration_time)");
    REQUIRE_FALSE(std::regex_search(code, snake_case));
  }
}

TEST_CASE("AddReference query uses correct columns", "[store][schema][compatibility]") {
  std::string code = read_file("src/nix/store/local-store.cpp");
  REQUIRE_FALSE(code.empty());

  SECTION("INSERT into Refs uses correct column names") {
    std::regex insert_query(R"(insert or replace into Refs\s*\(referrer, reference\))");
    REQUIRE(std::regex_search(code, insert_query));
  }
}

TEST_CASE("AddDerivationOutput query uses correct columns", "[store][schema][compatibility]") {
  std::string code = read_file("src/nix/store/local-store.cpp");
  REQUIRE_FALSE(code.empty());

  SECTION("INSERT into DerivationOutputs uses correct column names") {
    std::regex insert_query(R"(insert or replace into DerivationOutputs\s*\(drv, id, path\))");
    REQUIRE(std::regex_search(code, insert_query));
  }
}

TEST_CASE("Realisations queries use camelCase column names", "[store][schema][compatibility]") {
  std::string code = read_file("src/nix/store/local-store.cpp");
  REQUIRE_FALSE(code.empty());

  SECTION("RegisterRealisedOutput INSERT uses camelCase") {
    std::regex insert_query(R"(insert into Realisations\s*\(drvPath, outputName, outputPath)");
    REQUIRE(std::regex_search(code, insert_query));
  }

  SECTION("RegisterRealisedOutput INSERT does not use snake_case") {
    std::regex snake_case(R"(insert into Realisations\s*\([^)]*drv_path)");
    REQUIRE_FALSE(std::regex_search(code, snake_case));

    std::regex snake_case2(R"(insert into Realisations\s*\([^)]*output_name)");
    REQUIRE_FALSE(std::regex_search(code, snake_case2));

    std::regex snake_case3(R"(insert into Realisations\s*\([^)]*output_path)");
    REQUIRE_FALSE(std::regex_search(code, snake_case3));
  }

  SECTION("UpdateRealisedOutput uses camelCase") {
    std::regex update_query(R"(update Realisations[^;]*drvPath\s*=)");
    REQUIRE(std::regex_search(code, update_query));
  }

  SECTION("QueryRealisedOutput SELECT uses camelCase") {
    std::regex select_query(R"(select[^;]*from Realisations[^;]*drvPath\s*=)");
    REQUIRE(std::regex_search(code, select_query));
  }

  SECTION("QueryAllRealisedOutputs uses outputName") {
    std::regex select_query(R"(select outputName[^;]*from Realisations)");
    REQUIRE(std::regex_search(code, select_query));
  }

  SECTION("QueryRealisationReferences uses camelCase") {
    std::regex select_query(R"(select drvPath, outputName from Realisations)");
    REQUIRE(std::regex_search(code, select_query));
  }

  SECTION("AddRealisationReference uses camelCase") {
    std::regex insert_query(
        R"(insert or replace into RealisationsRefs\s*\(referrer, realisationReference\))");
    REQUIRE(std::regex_search(code, insert_query));
  }
}

TEST_CASE("Binary cache queries use camelCase column names", "[store][schema][compatibility]") {
  std::string code = read_file("src/nix/store/nar-info-disk-cache.cpp");
  REQUIRE_FALSE(code.empty());

  SECTION("insert_cache uses camelCase") {
    std::regex insert_query(R"(insert into BinaryCaches[^;]*storeDir[^;]*wantMassQuery)");
    REQUIRE(std::regex_search(code, insert_query));
  }

  SECTION("query_cache uses camelCase") {
    // Query is: select id, storeDir, wantMassQuery, priority from BinaryCaches
    std::regex select_query(R"(select\s+id,\s*storeDir,\s*wantMassQuery)");
    REQUIRE(std::regex_search(code, select_query));
  }

  SECTION("insert_nar uses camelCase") {
    std::regex insert_query(R"(insert or replace into NARs[^;]*hashPart[^;]*namePart)");
    REQUIRE(std::regex_search(code, insert_query));
  }

  SECTION("insert_nar includes all camelCase columns") {
    std::regex insert_query(R"(insert or replace into NARs[^;]*fileHash[^;]*fileSize[^;]*narHash)");
    REQUIRE(std::regex_search(code, insert_query));
  }

  SECTION("query_nar uses camelCase") {
    std::regex select_query(R"(select[^;]*namePart[^;]*fileHash[^;]*fileSize[^;]*narHash)");
    REQUIRE(std::regex_search(code, select_query));
  }

  SECTION("insert_realisation uses outputId") {
    std::regex insert_query(R"(insert or replace into Realisations[^;]*outputId)");
    REQUIRE(std::regex_search(code, insert_query));
  }

  SECTION("query_realisation uses outputId") {
    std::regex select_query(R"(from Realisations[^;]*outputId\s*=)");
    REQUIRE(std::regex_search(code, select_query));
  }
}

// =============================================================================
// SQLite schema version compatibility
// =============================================================================

TEST_CASE("Schema version migrations use correct column names", "[store][schema][compatibility]") {
  std::string code = read_file("src/nix/store/local-store.cpp");
  REQUIRE_FALSE(code.empty());

  SECTION("Schema version 8 migration adds ultimate column") {
    std::regex migration(R"(alter table ValidPaths add column ultimate)");
    REQUIRE(std::regex_search(code, migration));
  }

  SECTION("Schema version 8 migration adds sigs column") {
    std::regex migration(R"(alter table ValidPaths add column sigs)");
    REQUIRE(std::regex_search(code, migration));
  }

  SECTION("Schema version 10 migration adds ca column") {
    std::regex migration(R"(alter table ValidPaths add column ca)");
    REQUIRE(std::regex_search(code, migration));
  }

  SECTION("SchemaMigrations table exists") {
    std::regex table(R"(create table if not exists SchemaMigrations)");
    REQUIRE(std::regex_search(code, table));
  }

  SECTION("CA derivations migration is named correctly") {
    std::regex migration(R"(20220326-ca-derivations)");
    REQUIRE(std::regex_search(code, migration));
  }
}
