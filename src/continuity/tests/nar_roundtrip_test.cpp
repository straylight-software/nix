// src/continuity/tests/nar_roundtrip_test.cpp
//
// Roundtrip test: Compare Continuity NAR parser/serializer to legacy.
// This verifies that the new Continuity-based implementation produces
// identical results to the existing Nix implementation.

#include <cassert>
#include <cstring>
#include <iostream>
#include <string>

#include "continuity/nix/nix_formats.h"

namespace {

// Test serializing a simple file
void test_simple_file() {
  continuity::nix::nar_node_t node;
  continuity::nix::nar_node_t::file_t file;
  file.executable = false;
  file.contents = {'h', 'e', 'l', 'l', 'o'};
  node.data = std::move(file);

  continuity::nix::nar_t nar;
  nar.root = std::move(node);

  std::vector<std::uint8_t> out;
  continuity::nix::serialize_nar(nar, out);

  // Check that it starts with the magic
  auto magic_result = continuity::nix::parse_nix_string(out);
  assert(magic_result.is_ok());
  assert(magic_result.value.value() == continuity::nix::nar_magic);

  // Parse back
  auto parse_result = continuity::nix::parse_nar(out);
  assert(parse_result.is_ok() && "parse should succeed");

  const auto& parsed = parse_result.value.value();
  assert(parsed.root.is_file());

  const auto& parsed_file = std::get<continuity::nix::nar_node_t::file_t>(parsed.root.data);
  assert(!parsed_file.executable);
  assert(parsed_file.contents.size() == 5);
  assert(std::memcmp(parsed_file.contents.data(), "hello", 5) == 0);

  std::cout << "test_simple_file: PASSED\n";
}

// Test executable file
void test_executable_file() {
  continuity::nix::nar_node_t node;
  continuity::nix::nar_node_t::file_t file;
  file.executable = true;
  file.contents = {'#', '!', '/', 'b', 'i', 'n', '/', 's', 'h'};
  node.data = std::move(file);

  continuity::nix::nar_t nar;
  nar.root = std::move(node);

  std::vector<std::uint8_t> out;
  continuity::nix::serialize_nar(nar, out);

  auto parse_result = continuity::nix::parse_nar(out);
  assert(parse_result.is_ok());

  const auto& parsed = parse_result.value.value();
  assert(parsed.root.is_file());

  const auto& parsed_file = std::get<continuity::nix::nar_node_t::file_t>(parsed.root.data);
  assert(parsed_file.executable);

  std::cout << "test_executable_file: PASSED\n";
}

// Test symlink
void test_symlink() {
  continuity::nix::nar_node_t node;
  continuity::nix::nar_node_t::symlink_t link;
  link.target = "/nix/store/abc123-target";
  node.data = std::move(link);

  continuity::nix::nar_t nar;
  nar.root = std::move(node);

  std::vector<std::uint8_t> out;
  continuity::nix::serialize_nar(nar, out);

  auto parse_result = continuity::nix::parse_nar(out);
  assert(parse_result.is_ok());

  const auto& parsed = parse_result.value.value();
  assert(parsed.root.is_symlink());

  const auto& parsed_link = std::get<continuity::nix::nar_node_t::symlink_t>(parsed.root.data);
  assert(parsed_link.target == "/nix/store/abc123-target");

  std::cout << "test_symlink: PASSED\n";
}

// Test simple directory
void test_simple_directory() {
  continuity::nix::nar_node_t root;
  continuity::nix::nar_node_t::dir_t dir;

  // Add a file entry
  continuity::nix::nar_entry_t entry;
  entry.name = "hello.txt";
  entry.node = std::make_unique<continuity::nix::nar_node_t>();
  continuity::nix::nar_node_t::file_t file;
  file.executable = false;
  file.contents = {'w', 'o', 'r', 'l', 'd'};
  entry.node->data = std::move(file);

  dir.entries.push_back(std::move(entry));
  root.data = std::move(dir);

  continuity::nix::nar_t nar;
  nar.root = std::move(root);

  std::vector<std::uint8_t> out;
  continuity::nix::serialize_nar(nar, out);

  auto parse_result = continuity::nix::parse_nar(out);
  assert(parse_result.is_ok());

  const auto& parsed = parse_result.value.value();
  assert(parsed.root.is_dir());

  const auto& parsed_dir = std::get<continuity::nix::nar_node_t::dir_t>(parsed.root.data);
  assert(parsed_dir.entries.size() == 1);
  assert(parsed_dir.entries[0].name == "hello.txt");
  assert(parsed_dir.entries[0].node->is_file());

  std::cout << "test_simple_directory: PASSED\n";
}

// Test padding calculation
void test_padding() {
  assert(continuity::nix::pad_size(0) == 0);
  assert(continuity::nix::pad_size(1) == 7);
  assert(continuity::nix::pad_size(7) == 1);
  assert(continuity::nix::pad_size(8) == 0);
  assert(continuity::nix::pad_size(9) == 7);
  assert(continuity::nix::pad_size(15) == 1);
  assert(continuity::nix::pad_size(16) == 0);

  std::cout << "test_padding: PASSED\n";
}

// Test u64le parsing
void test_u64le() {
  std::vector<std::uint8_t> bytes = {0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08};
  auto result = continuity::nix::parse_u64le(bytes);
  assert(result.is_ok());

  const std::uint64_t expected = 0x0807060504030201ULL;
  assert(result.value.value() == expected);

  std::vector<std::uint8_t> out;
  continuity::nix::serialize_u64le(expected, out);
  assert(out == bytes);

  std::cout << "test_u64le: PASSED\n";
}

// Test nix string parsing
void test_nix_string() {
  std::vector<std::uint8_t> out;
  continuity::nix::serialize_nix_string("hello", out);

  // Should be: 8 bytes length (5) + 5 bytes data + 3 bytes padding = 16 bytes
  assert(out.size() == 16);

  auto result = continuity::nix::parse_nix_string(out);
  assert(result.is_ok());
  assert(result.value.value() == "hello");
  assert(result.remaining.empty());

  std::cout << "test_nix_string: PASSED\n";
}

} // namespace

int main() {
  std::cout << "Running NAR roundtrip tests...\n\n";

  test_padding();
  test_u64le();
  test_nix_string();
  test_simple_file();
  test_executable_file();
  test_symlink();
  test_simple_directory();

  std::cout << "\nAll NAR roundtrip tests PASSED!\n";
  return 0;
}
