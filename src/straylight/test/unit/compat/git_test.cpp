// straylight::nix::compat::git tests
//
// Tests for Git object parsing primitives

#include <array>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

using namespace std::string_literals;

#include <catch2/catch_test_macros.hpp>

#include "straylight/nix/compat/git.h"

namespace git = straylight::nix::compat;

// ─────────────────────────────────────────────────────────────────────────────
// Helper functions
// ─────────────────────────────────────────────────────────────────────────────

namespace {

/// Create a raw git object with header.
std::vector<std::uint8_t> make_raw_object(std::string_view type, std::string_view content) {
  std::string header = std::string(type) + " " + std::to_string(content.size());
  std::vector<std::uint8_t> result;
  result.reserve(header.size() + 1 + content.size());
  result.insert(result.end(), header.begin(), header.end());
  result.push_back('\0');
  result.insert(result.end(), content.begin(), content.end());
  return result;
}

/// Create a raw tree entry (mode + space + name + null + hash).
std::vector<std::uint8_t> make_tree_entry(std::string_view mode, std::string_view name,
                                          const git::GitHash& hash) {
  std::vector<std::uint8_t> entry;
  entry.insert(entry.end(), mode.begin(), mode.end());
  entry.push_back(' ');
  entry.insert(entry.end(), name.begin(), name.end());
  entry.push_back('\0');
  const auto& hash_bytes = hash.bytes();
  entry.insert(entry.end(), hash_bytes.begin(), hash_bytes.end());
  return entry;
}

} // namespace

// ─────────────────────────────────────────────────────────────────────────────
// GitHash - construction and conversion
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("GitHash default constructor creates zero hash", "[git][hash]") {
  git::GitHash hash;
  REQUIRE(hash.is_zero());
  REQUIRE(hash.to_hex() == "0000000000000000000000000000000000000000");
}

TEST_CASE("GitHash from array constructor", "[git][hash]") {
  std::array<std::uint8_t, 20> bytes = {0xde, 0xad, 0xbe, 0xef, 0x00, 0x11, 0x22, 0x33, 0x44, 0x55,
                                        0x66, 0x77, 0x88, 0x99, 0xaa, 0xbb, 0xcc, 0xdd, 0xee, 0xff};
  git::GitHash hash(bytes);
  REQUIRE_FALSE(hash.is_zero());
  REQUIRE(hash.to_hex() == "deadbeef00112233445566778899aabbccddeeff");
}

TEST_CASE("GitHash from_hex with valid lowercase hex", "[git][hash]") {
  auto hash = git::GitHash::from_hex("deadbeef00112233445566778899aabbccddeeff");
  REQUIRE(hash.has_value());
  REQUIRE(hash->to_hex() == "deadbeef00112233445566778899aabbccddeeff");
}

TEST_CASE("GitHash from_hex with valid uppercase hex", "[git][hash]") {
  auto hash = git::GitHash::from_hex("DEADBEEF00112233445566778899AABBCCDDEEFF");
  REQUIRE(hash.has_value());
  REQUIRE(hash->to_hex() == "deadbeef00112233445566778899aabbccddeeff");
}

TEST_CASE("GitHash from_hex with mixed case hex", "[git][hash]") {
  auto hash = git::GitHash::from_hex("DeAdBeEf00112233445566778899AaBbCcDdEeFf");
  REQUIRE(hash.has_value());
  REQUIRE(hash->to_hex() == "deadbeef00112233445566778899aabbccddeeff");
}

TEST_CASE("GitHash from_hex with invalid length returns nullopt", "[git][hash]") {
  auto too_short = git::GitHash::from_hex("deadbeef");
  REQUIRE_FALSE(too_short.has_value());

  auto too_long = git::GitHash::from_hex("deadbeef00112233445566778899aabbccddeeff00");
  REQUIRE_FALSE(too_long.has_value());
}

TEST_CASE("GitHash from_hex with invalid characters returns nullopt", "[git][hash]") {
  auto invalid = git::GitHash::from_hex("deadbeef00112233445566778899aabbccddeefg");
  REQUIRE_FALSE(invalid.has_value());

  auto with_spaces = git::GitHash::from_hex("deadbeef 0112233445566778899aabbccddeeff");
  REQUIRE_FALSE(with_spaces.has_value());
}

TEST_CASE("GitHash comparison operators", "[git][hash]") {
  auto hash1 = git::GitHash::from_hex("0000000000000000000000000000000000000001");
  auto hash2 = git::GitHash::from_hex("0000000000000000000000000000000000000002");
  auto hash1_copy = git::GitHash::from_hex("0000000000000000000000000000000000000001");

  REQUIRE(hash1 == hash1_copy);
  REQUIRE(hash1 != hash2);
  REQUIRE(hash1 < hash2);
  REQUIRE(hash2 > hash1);
}

TEST_CASE("GitHash bytes() returns raw bytes", "[git][hash]") {
  auto hash = git::GitHash::from_hex("deadbeef00112233445566778899aabbccddeeff");
  REQUIRE(hash.has_value());

  const auto& bytes = hash->bytes();
  REQUIRE(bytes[0] == 0xde);
  REQUIRE(bytes[1] == 0xad);
  REQUIRE(bytes[2] == 0xbe);
  REQUIRE(bytes[3] == 0xef);
}

// ─────────────────────────────────────────────────────────────────────────────
// GitObjectType - string conversion
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("GitObjectType to string conversion", "[git][object_type]") {
  REQUIRE(git::git_object_type_to_string(git::GitObjectType::blob) == "blob");
  REQUIRE(git::git_object_type_to_string(git::GitObjectType::tree) == "tree");
  REQUIRE(git::git_object_type_to_string(git::GitObjectType::commit) == "commit");
  REQUIRE(git::git_object_type_to_string(git::GitObjectType::tag) == "tag");
}

TEST_CASE("GitObjectType from string conversion", "[git][object_type]") {
  REQUIRE(git::git_object_type_from_string("blob") == git::GitObjectType::blob);
  REQUIRE(git::git_object_type_from_string("tree") == git::GitObjectType::tree);
  REQUIRE(git::git_object_type_from_string("commit") == git::GitObjectType::commit);
  REQUIRE(git::git_object_type_from_string("tag") == git::GitObjectType::tag);
}

TEST_CASE("GitObjectType from string with invalid type returns nullopt", "[git][object_type]") {
  REQUIRE_FALSE(git::git_object_type_from_string("invalid").has_value());
  REQUIRE_FALSE(git::git_object_type_from_string("").has_value());
  REQUIRE_FALSE(git::git_object_type_from_string("BLOB").has_value());
}

// ─────────────────────────────────────────────────────────────────────────────
// GitTreeEntryMode - parsing and helpers
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("GitTreeEntryMode from octal string", "[git][tree_entry_mode]") {
  REQUIRE(git::git_tree_entry_mode_from_octal("40000") == git::GitTreeEntryMode::directory);
  REQUIRE(git::git_tree_entry_mode_from_octal("100644") == git::GitTreeEntryMode::regular_file);
  REQUIRE(git::git_tree_entry_mode_from_octal("100755") == git::GitTreeEntryMode::executable_file);
  REQUIRE(git::git_tree_entry_mode_from_octal("120000") == git::GitTreeEntryMode::symbolic_link);
  REQUIRE(git::git_tree_entry_mode_from_octal("160000") == git::GitTreeEntryMode::gitlink);
}

TEST_CASE("GitTreeEntryMode from invalid octal returns nullopt", "[git][tree_entry_mode]") {
  REQUIRE_FALSE(git::git_tree_entry_mode_from_octal("123456").has_value());
  REQUIRE_FALSE(git::git_tree_entry_mode_from_octal("").has_value());
  REQUIRE_FALSE(git::git_tree_entry_mode_from_octal("notoctal").has_value());
}

TEST_CASE("is_directory_mode helper", "[git][tree_entry_mode]") {
  REQUIRE(git::is_directory_mode(git::GitTreeEntryMode::directory));
  REQUIRE_FALSE(git::is_directory_mode(git::GitTreeEntryMode::regular_file));
  REQUIRE_FALSE(git::is_directory_mode(git::GitTreeEntryMode::executable_file));
}

TEST_CASE("is_file_mode helper", "[git][tree_entry_mode]") {
  REQUIRE(git::is_file_mode(git::GitTreeEntryMode::regular_file));
  REQUIRE(git::is_file_mode(git::GitTreeEntryMode::executable_file));
  REQUIRE_FALSE(git::is_file_mode(git::GitTreeEntryMode::directory));
  REQUIRE_FALSE(git::is_file_mode(git::GitTreeEntryMode::symbolic_link));
}

// ─────────────────────────────────────────────────────────────────────────────
// GitParseError - string conversion
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("GitParseError to string conversion", "[git][parse_error]") {
  REQUIRE(git::git_parse_error_to_string(git::GitParseError::invalid_header) ==
          "invalid object header");
  REQUIRE(git::git_parse_error_to_string(git::GitParseError::truncated_data) == "truncated data");
  REQUIRE(git::git_parse_error_to_string(git::GitParseError::invalid_signature) ==
          "invalid signature format");
}

// ─────────────────────────────────────────────────────────────────────────────
// parse_git_object_header
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("parse_git_object_header with valid blob header", "[git][header]") {
  std::string header = "blob 123\0content"s;
  std::vector<std::uint8_t> data(header.begin(), header.end());

  auto result = git::parse_git_object_header(data);
  REQUIRE(result.has_value());
  REQUIRE(result->first.type == git::GitObjectType::blob);
  REQUIRE(result->first.size == 123);
  REQUIRE(result->second == 9); // Position after null byte
}

TEST_CASE("parse_git_object_header with valid tree header", "[git][header]") {
  std::string header = "tree 456\0"s;
  std::vector<std::uint8_t> data(header.begin(), header.end());

  auto result = git::parse_git_object_header(data);
  REQUIRE(result.has_value());
  REQUIRE(result->first.type == git::GitObjectType::tree);
  REQUIRE(result->first.size == 456);
}

TEST_CASE("parse_git_object_header with valid commit header", "[git][header]") {
  std::string header = "commit 789\0"s;
  std::vector<std::uint8_t> data(header.begin(), header.end());

  auto result = git::parse_git_object_header(data);
  REQUIRE(result.has_value());
  REQUIRE(result->first.type == git::GitObjectType::commit);
  REQUIRE(result->first.size == 789);
}

TEST_CASE("parse_git_object_header with missing null byte", "[git][header]") {
  std::string header = "blob 123";
  std::vector<std::uint8_t> data(header.begin(), header.end());

  auto result = git::parse_git_object_header(data);
  REQUIRE_FALSE(result.has_value());
  REQUIRE(result.error() == git::GitParseError::invalid_header);
}

TEST_CASE("parse_git_object_header with missing space", "[git][header]") {
  std::string header = "blob123\0"s;
  std::vector<std::uint8_t> data(header.begin(), header.end());

  auto result = git::parse_git_object_header(data);
  REQUIRE_FALSE(result.has_value());
  REQUIRE(result.error() == git::GitParseError::invalid_header);
}

TEST_CASE("parse_git_object_header with invalid type", "[git][header]") {
  std::string header = "invalid 123\0"s;
  std::vector<std::uint8_t> data(header.begin(), header.end());

  auto result = git::parse_git_object_header(data);
  REQUIRE_FALSE(result.has_value());
  REQUIRE(result.error() == git::GitParseError::invalid_object_type);
}

TEST_CASE("parse_git_object_header with invalid size", "[git][header]") {
  std::string header = "blob abc\0"s;
  std::vector<std::uint8_t> data(header.begin(), header.end());

  auto result = git::parse_git_object_header(data);
  REQUIRE_FALSE(result.has_value());
  REQUIRE(result.error() == git::GitParseError::invalid_size);
}

// ─────────────────────────────────────────────────────────────────────────────
// parse_git_object with blob
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("parse_git_object parses blob", "[git][blob]") {
  std::string content = "Hello, World!";
  auto data = make_raw_object("blob", content);

  auto result = git::parse_git_object(data);
  REQUIRE(result.has_value());
  REQUIRE(std::holds_alternative<git::GitBlob>(*result));

  const auto& blob = std::get<git::GitBlob>(*result);
  std::string blob_content(blob.content.begin(), blob.content.end());
  REQUIRE(blob_content == content);
}

TEST_CASE("parse_git_object parses empty blob", "[git][blob]") {
  auto data = make_raw_object("blob", "");

  auto result = git::parse_git_object(data);
  REQUIRE(result.has_value());
  REQUIRE(std::holds_alternative<git::GitBlob>(*result));

  const auto& blob = std::get<git::GitBlob>(*result);
  REQUIRE(blob.content.empty());
}

TEST_CASE("parse_git_object parses binary blob", "[git][blob]") {
  std::vector<std::uint8_t> binary_content = {0x00, 0x01, 0x02, 0xff, 0xfe, 0xfd};
  std::string header = "blob 6\0"s;
  std::vector<std::uint8_t> data(header.begin(), header.end());
  data.insert(data.end(), binary_content.begin(), binary_content.end());

  auto result = git::parse_git_object(data);
  REQUIRE(result.has_value());
  REQUIRE(std::holds_alternative<git::GitBlob>(*result));

  const auto& blob = std::get<git::GitBlob>(*result);
  REQUIRE(blob.content == binary_content);
}

// ─────────────────────────────────────────────────────────────────────────────
// parse_git_object with tree
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("parse_git_object parses tree with single file entry", "[git][tree]") {
  auto file_hash = git::GitHash::from_hex("deadbeef00112233445566778899aabbccddeeff");
  REQUIRE(file_hash.has_value());

  auto entry = make_tree_entry("100644", "file.txt", *file_hash);

  std::string header = "tree " + std::to_string(entry.size()) + '\0';
  std::vector<std::uint8_t> data(header.begin(), header.end());
  data.insert(data.end(), entry.begin(), entry.end());

  auto result = git::parse_git_object(data);
  REQUIRE(result.has_value());
  REQUIRE(std::holds_alternative<git::GitTree>(*result));

  const auto& tree = std::get<git::GitTree>(*result);
  REQUIRE(tree.entries.size() == 1);
  REQUIRE(tree.entries[0].mode == git::GitTreeEntryMode::regular_file);
  REQUIRE(tree.entries[0].name == "file.txt");
  REQUIRE(tree.entries[0].hash == *file_hash);
}

TEST_CASE("parse_git_object parses tree with directory entry", "[git][tree]") {
  auto dir_hash = git::GitHash::from_hex("0123456789abcdef0123456789abcdef01234567");
  REQUIRE(dir_hash.has_value());

  auto entry = make_tree_entry("40000", "subdir", *dir_hash);

  std::string header = "tree " + std::to_string(entry.size()) + '\0';
  std::vector<std::uint8_t> data(header.begin(), header.end());
  data.insert(data.end(), entry.begin(), entry.end());

  auto result = git::parse_git_object(data);
  REQUIRE(result.has_value());
  REQUIRE(std::holds_alternative<git::GitTree>(*result));

  const auto& tree = std::get<git::GitTree>(*result);
  REQUIRE(tree.entries.size() == 1);
  REQUIRE(tree.entries[0].mode == git::GitTreeEntryMode::directory);
  REQUIRE(tree.entries[0].name == "subdir");
}

TEST_CASE("parse_git_object parses tree with multiple entries", "[git][tree]") {
  auto hash1 = git::GitHash::from_hex("1111111111111111111111111111111111111111");
  auto hash2 = git::GitHash::from_hex("2222222222222222222222222222222222222222");
  auto hash3 = git::GitHash::from_hex("3333333333333333333333333333333333333333");
  REQUIRE(hash1.has_value());
  REQUIRE(hash2.has_value());
  REQUIRE(hash3.has_value());

  auto entry1 = make_tree_entry("100644", "a.txt", *hash1);
  auto entry2 = make_tree_entry("100755", "script.sh", *hash2);
  auto entry3 = make_tree_entry("40000", "dir", *hash3);

  std::vector<std::uint8_t> entries;
  entries.insert(entries.end(), entry1.begin(), entry1.end());
  entries.insert(entries.end(), entry2.begin(), entry2.end());
  entries.insert(entries.end(), entry3.begin(), entry3.end());

  std::string header = "tree " + std::to_string(entries.size()) + '\0';
  std::vector<std::uint8_t> data(header.begin(), header.end());
  data.insert(data.end(), entries.begin(), entries.end());

  auto result = git::parse_git_object(data);
  REQUIRE(result.has_value());
  REQUIRE(std::holds_alternative<git::GitTree>(*result));

  const auto& tree = std::get<git::GitTree>(*result);
  REQUIRE(tree.entries.size() == 3);
  REQUIRE(tree.entries[0].name == "a.txt");
  REQUIRE(tree.entries[0].mode == git::GitTreeEntryMode::regular_file);
  REQUIRE(tree.entries[1].name == "script.sh");
  REQUIRE(tree.entries[1].mode == git::GitTreeEntryMode::executable_file);
  REQUIRE(tree.entries[2].name == "dir");
  REQUIRE(tree.entries[2].mode == git::GitTreeEntryMode::directory);
}

TEST_CASE("parse_git_object parses empty tree", "[git][tree]") {
  auto data = make_raw_object("tree", "");

  auto result = git::parse_git_object(data);
  REQUIRE(result.has_value());
  REQUIRE(std::holds_alternative<git::GitTree>(*result));

  const auto& tree = std::get<git::GitTree>(*result);
  REQUIRE(tree.entries.empty());
}

// ─────────────────────────────────────────────────────────────────────────────
// parse_git_object with commit
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("parse_git_object parses simple commit", "[git][commit]") {
  std::string commit_content = "tree 4b825dc642cb6eb9a060e54bf8d69288fbee4904\n"
                               "author John Doe <john@example.com> 1234567890 +0000\n"
                               "committer Jane Doe <jane@example.com> 1234567891 -0500\n"
                               "\n"
                               "Initial commit\n";

  auto data = make_raw_object("commit", commit_content);

  auto result = git::parse_git_object(data);
  REQUIRE(result.has_value());
  REQUIRE(std::holds_alternative<git::GitCommit>(*result));

  const auto& commit = std::get<git::GitCommit>(*result);
  REQUIRE(commit.tree_hash.to_hex() == "4b825dc642cb6eb9a060e54bf8d69288fbee4904");
  REQUIRE(commit.parent_hashes.empty());
  REQUIRE(commit.author.name == "John Doe");
  REQUIRE(commit.author.email == "john@example.com");
  REQUIRE(commit.author.timestamp == 1234567890);
  REQUIRE(commit.author.timezone_offset == 0);
  REQUIRE(commit.committer.name == "Jane Doe");
  REQUIRE(commit.committer.email == "jane@example.com");
  REQUIRE(commit.committer.timestamp == 1234567891);
  REQUIRE(commit.committer.timezone_offset == -300); // -5 hours in minutes
  REQUIRE(commit.message == "Initial commit\n");
}

TEST_CASE("parse_git_object parses commit with single parent", "[git][commit]") {
  std::string commit_content = "tree 4b825dc642cb6eb9a060e54bf8d69288fbee4904\n"
                               "parent 1234567890abcdef1234567890abcdef12345678\n"
                               "author Test <test@test.com> 1000000000 +0100\n"
                               "committer Test <test@test.com> 1000000000 +0100\n"
                               "\n"
                               "Second commit\n";

  auto data = make_raw_object("commit", commit_content);

  auto result = git::parse_git_object(data);
  REQUIRE(result.has_value());
  REQUIRE(std::holds_alternative<git::GitCommit>(*result));

  const auto& commit = std::get<git::GitCommit>(*result);
  REQUIRE(commit.parent_hashes.size() == 1);
  REQUIRE(commit.parent_hashes[0].to_hex() == "1234567890abcdef1234567890abcdef12345678");
}

TEST_CASE("parse_git_object parses merge commit with multiple parents", "[git][commit]") {
  std::string commit_content = "tree 4b825dc642cb6eb9a060e54bf8d69288fbee4904\n"
                               "parent 1111111111111111111111111111111111111111\n"
                               "parent 2222222222222222222222222222222222222222\n"
                               "author Test <test@test.com> 1000000000 +0000\n"
                               "committer Test <test@test.com> 1000000000 +0000\n"
                               "\n"
                               "Merge commit\n";

  auto data = make_raw_object("commit", commit_content);

  auto result = git::parse_git_object(data);
  REQUIRE(result.has_value());
  REQUIRE(std::holds_alternative<git::GitCommit>(*result));

  const auto& commit = std::get<git::GitCommit>(*result);
  REQUIRE(commit.parent_hashes.size() == 2);
  REQUIRE(commit.parent_hashes[0].to_hex() == "1111111111111111111111111111111111111111");
  REQUIRE(commit.parent_hashes[1].to_hex() == "2222222222222222222222222222222222222222");
}

TEST_CASE("parse_git_object parses commit with multi-line message", "[git][commit]") {
  std::string commit_content = "tree 4b825dc642cb6eb9a060e54bf8d69288fbee4904\n"
                               "author Test <test@test.com> 1000000000 +0000\n"
                               "committer Test <test@test.com> 1000000000 +0000\n"
                               "\n"
                               "Subject line\n"
                               "\n"
                               "Body paragraph 1.\n"
                               "\n"
                               "Body paragraph 2.\n";

  auto data = make_raw_object("commit", commit_content);

  auto result = git::parse_git_object(data);
  REQUIRE(result.has_value());
  REQUIRE(std::holds_alternative<git::GitCommit>(*result));

  const auto& commit = std::get<git::GitCommit>(*result);
  REQUIRE(commit.message == "Subject line\n\nBody paragraph 1.\n\nBody paragraph 2.\n");
}

TEST_CASE("parse_git_object parses commit with negative timezone", "[git][commit]") {
  std::string commit_content = "tree 4b825dc642cb6eb9a060e54bf8d69288fbee4904\n"
                               "author Test <test@test.com> 1000000000 -0800\n"
                               "committer Test <test@test.com> 1000000000 -0800\n"
                               "\n"
                               "Test\n";

  auto data = make_raw_object("commit", commit_content);

  auto result = git::parse_git_object(data);
  REQUIRE(result.has_value());
  REQUIRE(std::holds_alternative<git::GitCommit>(*result));

  const auto& commit = std::get<git::GitCommit>(*result);
  REQUIRE(commit.author.timezone_offset == -480); // -8 hours in minutes
}

TEST_CASE("parse_git_object fails on commit missing tree", "[git][commit]") {
  std::string commit_content = "author Test <test@test.com> 1000000000 +0000\n"
                               "committer Test <test@test.com> 1000000000 +0000\n"
                               "\n"
                               "No tree\n";

  auto data = make_raw_object("commit", commit_content);

  auto result = git::parse_git_object(data);
  REQUIRE_FALSE(result.has_value());
  REQUIRE(result.error() == git::GitParseError::missing_required_field);
}

TEST_CASE("parse_git_object fails on commit with invalid tree hash", "[git][commit]") {
  std::string commit_content = "tree invalid_hash\n"
                               "author Test <test@test.com> 1000000000 +0000\n"
                               "committer Test <test@test.com> 1000000000 +0000\n"
                               "\n"
                               "Bad hash\n";

  auto data = make_raw_object("commit", commit_content);

  auto result = git::parse_git_object(data);
  REQUIRE_FALSE(result.has_value());
  REQUIRE(result.error() == git::GitParseError::invalid_hash);
}

// ─────────────────────────────────────────────────────────────────────────────
// parse_git_object with tag
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("parse_git_object parses annotated tag", "[git][tag]") {
  std::string tag_content = "object 4b825dc642cb6eb9a060e54bf8d69288fbee4904\n"
                            "type commit\n"
                            "tag v1.0.0\n"
                            "tagger Release Bot <bot@example.com> 1234567890 +0000\n"
                            "\n"
                            "Version 1.0.0 release\n";

  auto data = make_raw_object("tag", tag_content);

  auto result = git::parse_git_object(data);
  REQUIRE(result.has_value());
  REQUIRE(std::holds_alternative<git::GitTag>(*result));

  const auto& tag = std::get<git::GitTag>(*result);
  REQUIRE(tag.object_hash.to_hex() == "4b825dc642cb6eb9a060e54bf8d69288fbee4904");
  REQUIRE(tag.object_type == git::GitObjectType::commit);
  REQUIRE(tag.tag_name == "v1.0.0");
  REQUIRE(tag.tagger.name == "Release Bot");
  REQUIRE(tag.tagger.email == "bot@example.com");
  REQUIRE(tag.message == "Version 1.0.0 release\n");
}

TEST_CASE("parse_git_object parses tag without tagger", "[git][tag]") {
  std::string tag_content = "object 4b825dc642cb6eb9a060e54bf8d69288fbee4904\n"
                            "type commit\n"
                            "tag lightweight-like\n"
                            "\n"
                            "Tag message\n";

  auto data = make_raw_object("tag", tag_content);

  auto result = git::parse_git_object(data);
  REQUIRE(result.has_value());
  REQUIRE(std::holds_alternative<git::GitTag>(*result));

  const auto& tag = std::get<git::GitTag>(*result);
  REQUIRE(tag.tag_name == "lightweight-like");
  REQUIRE(tag.tagger.name.empty());
}

TEST_CASE("parse_git_object parses tag pointing to tree", "[git][tag]") {
  std::string tag_content = "object 4b825dc642cb6eb9a060e54bf8d69288fbee4904\n"
                            "type tree\n"
                            "tag tree-tag\n"
                            "\n"
                            "Tag pointing to tree\n";

  auto data = make_raw_object("tag", tag_content);

  auto result = git::parse_git_object(data);
  REQUIRE(result.has_value());
  REQUIRE(std::holds_alternative<git::GitTag>(*result));

  const auto& tag = std::get<git::GitTag>(*result);
  REQUIRE(tag.object_type == git::GitObjectType::tree);
}

TEST_CASE("parse_git_object fails on tag missing object", "[git][tag]") {
  std::string tag_content = "type commit\n"
                            "tag no-object\n"
                            "\n"
                            "Missing object\n";

  auto data = make_raw_object("tag", tag_content);

  auto result = git::parse_git_object(data);
  REQUIRE_FALSE(result.has_value());
  REQUIRE(result.error() == git::GitParseError::missing_required_field);
}

// ─────────────────────────────────────────────────────────────────────────────
// parse_git_object edge cases
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("parse_git_object with string_view overload", "[git][parse]") {
  std::string data = "blob 5\0hello"s;

  auto result = git::parse_git_object(std::string_view(data));
  REQUIRE(result.has_value());
  REQUIRE(std::holds_alternative<git::GitBlob>(*result));
}

TEST_CASE("parse_git_object fails with truncated data", "[git][parse]") {
  std::string header = "blob 100\0short"s;
  std::vector<std::uint8_t> data(header.begin(), header.end());

  auto result = git::parse_git_object(data);
  REQUIRE_FALSE(result.has_value());
  REQUIRE(result.error() == git::GitParseError::truncated_data);
}

TEST_CASE("parse_git_object fails with empty data", "[git][parse]") {
  std::vector<std::uint8_t> data;

  auto result = git::parse_git_object(data);
  REQUIRE_FALSE(result.has_value());
  REQUIRE(result.error() == git::GitParseError::invalid_header);
}

// ─────────────────────────────────────────────────────────────────────────────
// GitSignature parsing edge cases
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("parse_git_object handles author with special characters in name", "[git][commit]") {
  std::string commit_content = "tree 4b825dc642cb6eb9a060e54bf8d69288fbee4904\n"
                               "author José García-López <jose@example.com> 1234567890 +0200\n"
                               "committer José García-López <jose@example.com> 1234567890 +0200\n"
                               "\n"
                               "Test\n";

  auto data = make_raw_object("commit", commit_content);

  auto result = git::parse_git_object(data);
  REQUIRE(result.has_value());
  REQUIRE(std::holds_alternative<git::GitCommit>(*result));

  const auto& commit = std::get<git::GitCommit>(*result);
  REQUIRE(commit.author.name == "José García-López");
  REQUIRE(commit.author.timezone_offset == 120); // +2 hours
}

TEST_CASE("parse_git_object handles empty commit message", "[git][commit]") {
  std::string commit_content = "tree 4b825dc642cb6eb9a060e54bf8d69288fbee4904\n"
                               "author Test <test@test.com> 1000000000 +0000\n"
                               "committer Test <test@test.com> 1000000000 +0000\n"
                               "\n";

  auto data = make_raw_object("commit", commit_content);

  auto result = git::parse_git_object(data);
  REQUIRE(result.has_value());
  REQUIRE(std::holds_alternative<git::GitCommit>(*result));

  const auto& commit = std::get<git::GitCommit>(*result);
  REQUIRE(commit.message.empty());
}

// ─────────────────────────────────────────────────────────────────────────────
// Tree entry with symbolic link
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("parse_git_object parses tree with symbolic link entry", "[git][tree]") {
  auto link_hash = git::GitHash::from_hex("abcdef1234567890abcdef1234567890abcdef12");
  REQUIRE(link_hash.has_value());

  auto entry = make_tree_entry("120000", "link", *link_hash);

  std::string header = "tree " + std::to_string(entry.size()) + '\0';
  std::vector<std::uint8_t> data(header.begin(), header.end());
  data.insert(data.end(), entry.begin(), entry.end());

  auto result = git::parse_git_object(data);
  REQUIRE(result.has_value());
  REQUIRE(std::holds_alternative<git::GitTree>(*result));

  const auto& tree = std::get<git::GitTree>(*result);
  REQUIRE(tree.entries.size() == 1);
  REQUIRE(tree.entries[0].mode == git::GitTreeEntryMode::symbolic_link);
  REQUIRE(tree.entries[0].name == "link");
}

// ─────────────────────────────────────────────────────────────────────────────
// Tree entry with gitlink (submodule)
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("parse_git_object parses tree with gitlink entry", "[git][tree]") {
  auto submodule_hash = git::GitHash::from_hex("fedcba0987654321fedcba0987654321fedcba09");
  REQUIRE(submodule_hash.has_value());

  auto entry = make_tree_entry("160000", "submodule", *submodule_hash);

  std::string header = "tree " + std::to_string(entry.size()) + '\0';
  std::vector<std::uint8_t> data(header.begin(), header.end());
  data.insert(data.end(), entry.begin(), entry.end());

  auto result = git::parse_git_object(data);
  REQUIRE(result.has_value());
  REQUIRE(std::holds_alternative<git::GitTree>(*result));

  const auto& tree = std::get<git::GitTree>(*result);
  REQUIRE(tree.entries.size() == 1);
  REQUIRE(tree.entries[0].mode == git::GitTreeEntryMode::gitlink);
  REQUIRE(tree.entries[0].name == "submodule");
}
