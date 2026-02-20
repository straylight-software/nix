// SPDX-License-Identifier: MIT
// Test serializers against captured binary data
//
// Compile:
//   g++ -std=c++23 -Wall -Wextra -Wpedantic -o test_serialize test_serialize.cpp
//
// Run:
//   ./test_serialize

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <sstream>

#include "nix_daemon_serialize.h"

using namespace straylight::protocol;

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
      std::cerr << "FAIL " << name << ": byte mismatch at offset " << idx << " (got 0x" << std::hex
                << static_cast<int>(generated[idx]) << ", expected 0x"
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

  std::cout << "Testing Nix Daemon Protocol Serializers\n";
  std::cout << "========================================\n\n";

  // Test 1: Client Hello
  {
    std::vector<std::byte> buf;
    Writer w{buf};
    write_client_hello(w, 0x0126); // version 1.38

    auto expected = read_file("src/straylight/nix/protocol/captures/client_hello.bin");
    if (compare(buf, expected, "client_hello"))
      ++passed;
    else
      ++failed;
  }

  // Test 2: Server Hello
  {
    std::vector<std::byte> buf;
    Writer w{buf};
    write_server_hello(w, 0x0126);

    auto expected = read_file("src/straylight/nix/protocol/captures/server_hello.bin");
    if (compare(buf, expected, "server_hello"))
      ++passed;
    else
      ++failed;
  }

  // Test 3: IsValidPath request
  {
    std::vector<std::byte> buf;
    Writer w{buf};

    // Read captured request to get the path
    auto captured = read_file("src/straylight/nix/protocol/captures/isvalidpath_request.bin");
    if (captured.size() >= 16) {
      // Parse path from capture: op(8) + len(8) + data + padding
      uint64_t len;
      std::memcpy(&len, captured.data() + 8, 8);
      std::string path(reinterpret_cast<const char*>(captured.data() + 16), len);

      write_is_valid_path_request(w, path);
      if (compare(buf, captured, "isvalidpath_request"))
        ++passed;
      else
        ++failed;
    }
  }

  // Test 4: QueryPathInfo request
  {
    std::vector<std::byte> buf;
    Writer w{buf};

    auto captured = read_file("src/straylight/nix/protocol/captures/querypathinfo_request.bin");
    if (captured.size() >= 16) {
      uint64_t len;
      std::memcpy(&len, captured.data() + 8, 8);
      std::string path(reinterpret_cast<const char*>(captured.data() + 16), len);

      write_query_path_info_request(w, path);
      if (compare(buf, captured, "querypathinfo_request"))
        ++passed;
      else
        ++failed;
    }
  }

  // Test 5: QueryReferrers request
  {
    std::vector<std::byte> buf;
    Writer w{buf};

    auto captured = read_file("src/straylight/nix/protocol/captures/queryreferrers_request.bin");
    if (captured.size() >= 16) {
      uint64_t len;
      std::memcpy(&len, captured.data() + 8, 8);
      std::string path(reinterpret_cast<const char*>(captured.data() + 16), len);

      write_query_referrers_request(w, path);
      if (compare(buf, captured, "queryreferrers_request"))
        ++passed;
      else
        ++failed;
    }
  }

  // Test 6: AddTempRoot request
  {
    std::vector<std::byte> buf;
    Writer w{buf};

    auto captured = read_file("src/straylight/nix/protocol/captures/addtemproot_request.bin");
    if (captured.size() >= 16) {
      uint64_t len;
      std::memcpy(&len, captured.data() + 8, 8);
      std::string path(reinterpret_cast<const char*>(captured.data() + 16), len);

      write_add_temp_root_request(w, path);
      if (compare(buf, captured, "addtemproot_request"))
        ++passed;
      else
        ++failed;
    }
  }

  // Test 7: AddIndirectRoot request
  {
    std::vector<std::byte> buf;
    Writer w{buf};

    auto captured = read_file("src/straylight/nix/protocol/captures/addindirectroot_request.bin");
    if (captured.size() >= 16) {
      uint64_t len;
      std::memcpy(&len, captured.data() + 8, 8);
      std::string path(reinterpret_cast<const char*>(captured.data() + 16), len);

      write_add_indirect_root_request(w, path);
      if (compare(buf, captured, "addindirectroot_request"))
        ++passed;
      else
        ++failed;
    }
  }

  // Test 8: FindRoots request (no payload)
  {
    std::vector<std::byte> buf;
    Writer w{buf};
    write_find_roots_request(w);

    auto captured = read_file("src/straylight/nix/protocol/captures/findroots_request.bin");
    if (compare(buf, captured, "findroots_request"))
      ++passed;
    else
      ++failed;
  }

  // Test 9: NarFromPath request (synthetic)
  {
    std::vector<std::byte> buf;
    Writer w{buf};

    auto captured = read_file("src/straylight/nix/protocol/captures/narfrompath_request.bin");
    if (captured.size() >= 16) {
      uint64_t len;
      std::memcpy(&len, captured.data() + 8, 8);
      std::string path(reinterpret_cast<const char*>(captured.data() + 16), len);

      write_nar_from_path_request(w, path);
      if (compare(buf, captured, "narfrompath_request"))
        ++passed;
      else
        ++failed;
    }
  }

  // Test 10: QueryMissing request
  {
    std::vector<std::byte> buf;
    Writer w{buf};

    auto captured = read_file("src/straylight/nix/protocol/captures/querymissing_request.bin");
    if (captured.size() >= 24) {
      // op(8) + num_paths(8) + paths...
      uint64_t num_paths;
      std::memcpy(&num_paths, captured.data() + 8, 8);

      std::vector<std::string> paths;
      size_t offset = 16;
      for (uint64_t idx = 0; idx < num_paths && offset < captured.size(); ++idx) {
        uint64_t len;
        std::memcpy(&len, captured.data() + offset, 8);
        paths.emplace_back(reinterpret_cast<const char*>(captured.data() + offset + 8), len);
        size_t padding = (8 - (len % 8)) % 8;
        offset += 8 + len + padding;
      }

      write_query_missing_request(w, paths);
      if (compare(buf, captured, "querymissing_request"))
        ++passed;
      else
        ++failed;
    }
  }

  // Test 11: BuildPaths request
  {
    std::vector<std::byte> buf;
    Writer w{buf};

    auto captured = read_file("src/straylight/nix/protocol/captures/buildpaths_request.bin");
    if (captured.size() >= 24) {
      uint64_t num_paths;
      std::memcpy(&num_paths, captured.data() + 8, 8);

      std::vector<std::string> paths;
      size_t offset = 16;
      for (uint64_t idx = 0; idx < num_paths && offset < captured.size() - 8; ++idx) {
        uint64_t len;
        std::memcpy(&len, captured.data() + offset, 8);
        paths.emplace_back(reinterpret_cast<const char*>(captured.data() + offset + 8), len);
        size_t padding = (8 - (len % 8)) % 8;
        offset += 8 + len + padding;
      }

      // Last 8 bytes is build mode
      uint64_t mode;
      std::memcpy(&mode, captured.data() + captured.size() - 8, 8);

      write_build_paths_request(w, paths, static_cast<BuildMode>(mode));
      if (compare(buf, captured, "buildpaths_request"))
        ++passed;
      else
        ++failed;
    }
  }

  // Test 12: BuildPathsWithResults request
  {
    std::vector<std::byte> buf;
    Writer w{buf};

    auto captured = read_file("src/straylight/nix/protocol/captures/buildpathswithresults_request.bin");
    if (captured.size() >= 24) {
      uint64_t num_paths;
      std::memcpy(&num_paths, captured.data() + 8, 8);

      std::vector<std::string> paths;
      size_t offset = 16;
      for (uint64_t idx = 0; idx < num_paths && offset < captured.size() - 8; ++idx) {
        uint64_t len;
        std::memcpy(&len, captured.data() + offset, 8);
        paths.emplace_back(reinterpret_cast<const char*>(captured.data() + offset + 8), len);
        size_t padding = (8 - (len % 8)) % 8;
        offset += 8 + len + padding;
      }

      uint64_t mode;
      std::memcpy(&mode, captured.data() + captured.size() - 8, 8);

      write_build_paths_with_results_request(w, paths, static_cast<BuildMode>(mode));
      if (compare(buf, captured, "buildpathswithresults_request"))
        ++passed;
      else
        ++failed;
    }
  }

  // Test 13: SetOptions request
  {
    std::vector<std::byte> buf;
    Writer w{buf};

    ClientSettings settings;
    settings.keep_failed_ = false;
    settings.keep_going_ = false;
    settings.try_fallback_ = false;
    settings.verbosity_ = 3;
    settings.max_build_jobs_ = 32;
    settings.max_silent_time_ = 0;
    settings.use_build_hook_ = true;
    settings.verbose_build_ = 7;
    settings.log_type_ = 0;
    settings.print_build_trace_ = 0;
    settings.build_cores_ = 0;
    settings.use_substitutes_ = true;
    settings.overrides_ = {{"extra-platforms", "aarch64-linux"},
                           {"sandbox", "false"},
                           {"substituters", "https://cache.nixos.org https://weyl-ai.cachix.org"},
                           {"trusted-public-keys",
                            "cache.nixos.org-1:6NCHdD59X431o0gWypbMrAURkbJ16ZPMQFGspcDShjY= "
                            "weyl-ai.cachix.org-1:cR0SpSAPw7wejZ21ep4SLojE77gp5F2os260eEWqTTw="}};

    write_set_options_request(w, settings, 38);

    auto captured = read_file("src/straylight/nix/protocol/captures/setoptions_request.bin");
    if (compare(buf, captured, "setoptions_request"))
      ++passed;
    else {
      ++failed;
      std::cout << "Generated:\n";
      hexdump(buf, 128);
      std::cout << "Expected:\n";
      hexdump(captured, 128);
    }
  }

  // Test 14: AddToStoreNar request
  {
    std::vector<std::byte> buf;
    Writer w{buf};

    AddToStoreNarRequest req;
    req.path_ = "/nix/store/v4wqkf7dq9619yyv1wbf1jvpw4dhbs2f-tf.txt";
    req.deriver_ = ""; // none
    req.nar_hash_ = "60e5106c2a1d4ef9f82b58df02be7a482f35438ff9e6556194ab02e355256352";
    req.references_ = {};
    req.registration_time_ = 0;
    req.nar_size_ = 136;
    req.ultimate_ = false;
    req.signatures_ = {};
    req.ca_ = "fixed:sha256:081b9nklhcc0gck2a6j821xij57djvnfxg1fps0f5l2bbpd1sgp1";
    req.repair_ = false;
    req.dont_check_sigs_ = false;

    write_add_to_store_nar_request(w, req);

    auto captured = read_file("src/straylight/nix/protocol/captures/addtostorenar_request.bin");
    if (compare(buf, captured, "addtostorenar_request"))
      ++passed;
    else {
      ++failed;
      std::cout << "Generated:\n";
      hexdump(buf, 128);
      std::cout << "Expected:\n";
      hexdump(captured, 128);
    }
  }

  // =======================================================================
  // Reader Tests
  // =======================================================================

  std::cout << "\n--- Reader Tests ---\n\n";

  // Test 15: Read server_hello
  {
    auto data = read_file("src/straylight/nix/protocol/captures/server_hello.bin");
    try {
      Reader r{data};
      auto hello = read_server_hello(r);
      if (hello.magic_ == WORKER_MAGIC_2 && hello.version_ == 0x0126) {
        std::cout << "PASS read_server_hello (magic=0x" << std::hex << hello.magic_
                  << ", version=0x" << hello.version_ << std::dec << ")\n";
        ++passed;
      } else {
        std::cerr << "FAIL read_server_hello: wrong values\n";
        ++failed;
      }
    } catch (const std::exception& e) {
      std::cerr << "FAIL read_server_hello: " << e.what() << "\n";
      ++failed;
    }
  }

  // Test 16: Read isvalidpath_response
  {
    auto data = read_file("src/straylight/nix/protocol/captures/isvalidpath_response.bin");
    try {
      Reader r{data};
      bool valid = read_is_valid_path_response(r);
      if (valid) {
        std::cout << "PASS read_isvalidpath_response (valid=true)\n";
        ++passed;
      } else {
        std::cerr << "FAIL read_isvalidpath_response: expected true\n";
        ++failed;
      }
    } catch (const std::exception& e) {
      std::cerr << "FAIL read_isvalidpath_response: " << e.what() << "\n";
      ++failed;
    }
  }

  // Test 17: Read querypathinfo_response
  {
    auto data = read_file("src/straylight/nix/protocol/captures/querypathinfo_response.bin");
    try {
      Reader r{data};
      auto info = read_query_path_info_response(r, 0x0126);
      if (info && info->deriver_.find("bash") != std::string::npos &&
          info->nar_hash_.find("f7b02ee0") == 0 && info->references_.size() == 2) {
        std::cout << "PASS read_querypathinfo_response (deriver=" << info->deriver_.substr(0, 40)
                  << "..., refs=" << info->references_.size() << ")\n";
        ++passed;
      } else {
        std::cerr << "FAIL read_querypathinfo_response: wrong values\n";
        ++failed;
      }
    } catch (const std::exception& e) {
      std::cerr << "FAIL read_querypathinfo_response: " << e.what() << "\n";
      ++failed;
    }
  }

  // Test 18: Read querymissing_response
  {
    auto data = read_file("src/straylight/nix/protocol/captures/querymissing_response.bin");
    try {
      Reader r{data};
      auto result = read_query_missing_response(r);
      if (result.will_build_.empty() && result.will_substitute_.empty() &&
          result.download_size_ == 0 && result.nar_size_ == 0) {
        std::cout << "PASS read_querymissing_response (all empty)\n";
        ++passed;
      } else {
        std::cerr << "FAIL read_querymissing_response: expected empty\n";
        ++failed;
      }
    } catch (const std::exception& e) {
      std::cerr << "FAIL read_querymissing_response: " << e.what() << "\n";
      ++failed;
    }
  }

  // Test 19: Read queryreferrers_response
  {
    auto data = read_file("src/straylight/nix/protocol/captures/queryreferrers_response.bin");
    try {
      Reader r{data};
      auto referrers = read_query_referrers_response(r);
      if (!referrers.empty() && referrers[0].find("/nix/store/") == 0) {
        std::cout << "PASS read_queryreferrers_response (count=" << referrers.size() << ")\n";
        ++passed;
      } else {
        std::cerr << "FAIL read_queryreferrers_response: empty or invalid\n";
        ++failed;
      }
    } catch (const std::exception& e) {
      std::cerr << "FAIL read_queryreferrers_response: " << e.what() << "\n";
      ++failed;
    }
  }

  // Test 20: Read buildpathswithresults_response
  {
    auto data = read_file("src/straylight/nix/protocol/captures/buildpathswithresults_response.bin");
    try {
      Reader r{data};
      auto results = read_build_paths_with_results_response(r, 0x0126);
      if (results.size() == 1 && results[0].path_.find("hello") != std::string::npos &&
          results[0].status_ == 2 && results[0].built_outputs_.size() == 1) {
        std::cout << "PASS read_buildpathswithresults_response (path="
                  << results[0].path_.substr(0, 30) << "..., status=" << results[0].status_
                  << ")\n";
        ++passed;
      } else {
        std::cerr << "FAIL read_buildpathswithresults_response: wrong values\n";
        ++failed;
      }
    } catch (const std::exception& e) {
      std::cerr << "FAIL read_buildpathswithresults_response: " << e.what() << "\n";
      ++failed;
    }
  }

  std::cout << "\n========================================\n";
  std::cout << "Results: " << passed << " passed, " << failed << " failed\n";

  return failed > 0 ? 1 : 0;
}
