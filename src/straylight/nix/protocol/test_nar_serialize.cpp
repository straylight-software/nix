// SPDX-License-Identifier: MIT
// Test NAR serializers against captured binary data
//
// Compile:
//   g++ -std=c++23 -Wall -Wextra -Wpedantic -o test_nar_serialize test_nar_serialize.cpp
//
// Run:
//   ./test_nar_serialize

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>

#include "nar_serialize.h"

namespace nar = straylight::nar;

// Read binary file
std::vector<std::byte> read_file(const char* path) {
  std::ifstream file(path, std::ios::binary | std::ios::ate);
  if (!file) {
    std::cerr << "Failed to open: " << path << "\n";
    return {};
  }
  auto size = file.tellg();
  file.seekg(0);
  std::vector<std::byte> data(size);
  file.read(reinterpret_cast<char*>(data.data()), size);
  return data;
}

// Compare buffers
bool compare(std::span<const std::byte> generated, std::span<const std::byte> expected,
             const char* name) {
  if (generated.size() != expected.size()) {
    std::cerr << "FAIL " << name << ": size mismatch (got " << generated.size() << ", expected "
              << expected.size() << ")\n";
    return false;
  }
  for (size_t idx = 0; idx < generated.size(); ++idx) {
    if (generated[idx] != expected[idx]) {
      std::cerr << "FAIL " << name << ": byte mismatch at offset 0x" << std::hex << idx
                << " (got 0x" << static_cast<int>(generated[idx]) << ", expected 0x"
                << static_cast<int>(expected[idx]) << std::dec << ")\n";
      return false;
    }
  }
  std::cout << "PASS " << name << " (" << generated.size() << " bytes)\n";
  return true;
}

// Hex dump for debugging
void hexdump(std::span<const std::byte> data, size_t limit = 64) {
  for (size_t idx = 0; idx < std::min(data.size(), limit); ++idx) {
    printf("%02x ", static_cast<uint8_t>(data[idx]));
    if ((idx + 1) % 16 == 0)
      printf("\n");
  }
  if (data.size() > limit)
    printf("... (%zu more bytes)\n", data.size() - limit);
  printf("\n");
}

int main() {
  int passed = 0, failed = 0;

  std::cout << "Testing NAR Serializers\n";
  std::cout << "=======================\n\n";

  // Test 1: Regular file
  // Captured: echo "hello world" > file && nix-store --dump file
  // Content is "hello world\n" (12 bytes including newline)
  {
    std::vector<std::byte> buf;
    nar::Writer w{buf};
    nar::dump_string(w, "hello world\n");

    auto expected = read_file("src/straylight/nix/protocol/nar_captures/regular_file.nar");
    if (compare(buf, expected, "regular_file"))
      ++passed;
    else {
      ++failed;
      std::cout << "Generated:\n";
      hexdump(buf);
      std::cout << "Expected:\n";
      hexdump(expected);
    }
  }

  // Test 2: Executable file
  // Captured: echo "#!/bin/bash" > script && chmod +x script && nix-store --dump script
  {
    std::vector<std::byte> buf;
    nar::Writer w{buf};
    nar::dump_executable(w, "#!/bin/bash\n");

    auto expected = read_file("src/straylight/nix/protocol/nar_captures/executable_file.nar");
    if (compare(buf, expected, "executable_file"))
      ++passed;
    else {
      ++failed;
      std::cout << "Generated:\n";
      hexdump(buf);
      std::cout << "Expected:\n";
      hexdump(expected);
    }
  }

  // Test 3: Symlink
  // Captured: ln -s /etc/passwd link && nix-store --dump link
  {
    std::vector<std::byte> buf;
    nar::Writer w{buf};
    nar::dump_symlink(w, "/etc/passwd");

    auto expected = read_file("src/straylight/nix/protocol/nar_captures/symlink.nar");
    if (compare(buf, expected, "symlink"))
      ++passed;
    else {
      ++failed;
      std::cout << "Generated:\n";
      hexdump(buf);
      std::cout << "Expected:\n";
      hexdump(expected);
    }
  }

  // Test 4: Empty file
  // Captured: touch empty && nix-store --dump empty
  {
    std::vector<std::byte> buf;
    nar::Writer w{buf};
    nar::dump_string(w, "");

    auto expected = read_file("src/straylight/nix/protocol/nar_captures/empty_file.nar");
    if (compare(buf, expected, "empty_file"))
      ++passed;
    else {
      ++failed;
      std::cout << "Generated:\n";
      hexdump(buf);
      std::cout << "Expected:\n";
      hexdump(expected);
    }
  }

  // Test 5: Empty directory
  // Captured: mkdir empty_dir && nix-store --dump empty_dir
  {
    std::vector<std::byte> buf;
    nar::Writer w{buf};
    nar::dump_empty_directory(w);

    auto expected = read_file("src/straylight/nix/protocol/nar_captures/empty_directory.nar");
    if (compare(buf, expected, "empty_directory"))
      ++passed;
    else {
      ++failed;
      std::cout << "Generated:\n";
      hexdump(buf);
      std::cout << "Expected:\n";
      hexdump(expected);
    }
  }

  // Test 6: Directory with files and subdirectory
  // Captured:
  //   mkdir -p dir/subdir
  //   echo "file1" > dir/a.txt
  //   echo "file2" > dir/b.txt
  //   echo "nested" > dir/subdir/c.txt
  //   nix-store --dump dir
  {
    auto tree = nar::FsObject::directory({
        {"a.txt", nar::FsObject::file("file1\n")},
        {"b.txt", nar::FsObject::file("file2\n")},
        {"subdir", nar::FsObject::directory({{"c.txt", nar::FsObject::file("nested\n")}})},
    });

    std::vector<std::byte> buf;
    tree.to_nar(buf);

    auto expected = read_file("src/straylight/nix/protocol/nar_captures/directory.nar");
    if (compare(buf, expected, "directory"))
      ++passed;
    else {
      ++failed;
      std::cout << "Generated:\n";
      hexdump(buf, 256);
      std::cout << "Expected:\n";
      hexdump(expected, 256);
    }
  }

  // =============================================================================
  // Property tests
  // =============================================================================

  std::cout << "\nProperty Tests\n";
  std::cout << "--------------\n";

  // Test 7: NAR always 8-byte aligned
  {
    bool all_aligned = true;
    for (size_t len = 0; len <= 50; ++len) {
      std::string content(len, 'x');
      std::vector<std::byte> buf;
      nar::Writer w{buf};
      nar::dump_string(w, content);

      if (buf.size() % 8 != 0) {
        std::cerr << "FAIL 8-byte alignment: length " << len << " produced " << buf.size()
                  << " bytes\n";
        all_aligned = false;
      }
    }
    if (all_aligned) {
      std::cout << "PASS 8-byte alignment (tested lengths 0-50)\n";
      ++passed;
    } else {
      ++failed;
    }
  }

  // Test 8: NAR is deterministic
  {
    bool deterministic = true;
    for (int idx = 0; idx < 10; ++idx) {
      std::vector<std::byte> buf1, buf2;
      nar::Writer w1{buf1}, w2{buf2};
      nar::dump_string(w1, "determinism test");
      nar::dump_string(w2, "determinism test");

      if (buf1 != buf2) {
        std::cerr << "FAIL determinism: iteration " << idx << " produced different output\n";
        deterministic = false;
      }
    }
    if (deterministic) {
      std::cout << "PASS determinism (10 iterations)\n";
      ++passed;
    } else {
      ++failed;
    }
  }

  // Test 9: Directory entries are sorted
  {
    // Create directory with unsorted entries
    auto tree1 = nar::FsObject::directory({
        {"z", nar::FsObject::file("z")},
        {"a", nar::FsObject::file("a")},
        {"m", nar::FsObject::file("m")},
    });

    // Create directory with sorted entries
    auto tree2 = nar::FsObject::directory({
        {"a", nar::FsObject::file("a")},
        {"m", nar::FsObject::file("m")},
        {"z", nar::FsObject::file("z")},
    });

    std::vector<std::byte> buf1, buf2;
    tree1.to_nar(buf1);
    tree2.to_nar(buf2);

    if (buf1 == buf2) {
      std::cout << "PASS directory entry sorting\n";
      ++passed;
    } else {
      std::cerr << "FAIL directory entry sorting: different output for sorted/unsorted input\n";
      ++failed;
    }
  }

  std::cout << "\nRound-trip Tests (parse captured NAR -> serialize -> compare)\n";
  std::cout << "--------------------------------------------------------------\n";

  // Test 10-15: Round-trip tests for each captured NAR
  const char* nar_files[] = {"regular_file.nar", "executable_file.nar", "symlink.nar",
                             "empty_file.nar",   "empty_directory.nar", "directory.nar"};

  for (const char* name : nar_files) {
    std::string path = std::string("src/straylight/nix/protocol/nar_captures/") + name;
    auto original = read_file(path.c_str());
    if (original.empty()) {
      std::cerr << "FAIL " << name << ": could not read file\n";
      ++failed;
      continue;
    }

    auto parse_result = nar::Reader::parse(original);
    if (nar::is_error(parse_result)) {
      std::cerr << "FAIL " << name << ": parse error: " << nar::get_error(parse_result).message_
                << "\n";
      ++failed;
      continue;
    }

    auto& obj = nar::get_value(parse_result);
    std::vector<std::byte> reserialized;
    obj.to_nar(reserialized);

    if (reserialized == original) {
      std::cout << "PASS roundtrip " << name << " (" << original.size() << " bytes)\n";
      ++passed;
    } else {
      std::cerr << "FAIL roundtrip " << name << ": size mismatch (got " << reserialized.size()
                << ", expected " << original.size() << ")\n";
      ++failed;
    }
  }

  std::cout << "\n=======================\n";
  std::cout << "Results: " << passed << " passed, " << failed << " failed\n";

  return failed > 0 ? 1 : 0;
}
