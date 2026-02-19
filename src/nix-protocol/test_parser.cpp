// Test harness for validating Kaitai-generated Nix daemon protocol parser
// Compile with:
//   g++ -std=c++23
//   -I/nix/store/dfksi7kh3ij6ar3yfc3641rfcz8kz0va-kaitai-struct-cpp-stl-runtime-0.11-dev/include \
//     -L/nix/store/n8gns27gjyc5gzkpqhiz27wcdalph44q-kaitai-struct-cpp-stl-runtime-0.11/lib \
//     -lkaitai_struct_cpp_stl_runtime \
//     -Wl,-rpath,/nix/store/n8gns27gjyc5gzkpqhiz27wcdalph44q-kaitai-struct-cpp-stl-runtime-0.11/lib
//     \ test_parser.cpp nix_daemon_protocol.cpp -o test_parser

#include <cassert>
#include <cstdint>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <memory>
#include <sstream>
#include <vector>

#include "kaitai/kaitaistream.h"
#include "nix_daemon_protocol.h"

namespace {

constexpr uint16_t PROTOCOL_VERSION_1_38 = 0x0126; // 1.38

// WORKER_MAGIC_1 as 4-byte string "nixc" in little-endian appears as "cxin"
const std::string WORKER_MAGIC_1_STR = "cxin"; // "nixc" read as bytes
// WORKER_MAGIC_2 as 4-byte string "oixd" (little-endian "dxio")
const std::string WORKER_MAGIC_2_STR = "oixd";

// Helper to read file into vector
std::vector<uint8_t> read_file(const std::string& path) {
  std::ifstream file(path, std::ios::binary | std::ios::ate);
  if (!file) {
    throw std::runtime_error("Cannot open file: " + path);
  }
  auto size = file.tellg();
  file.seekg(0, std::ios::beg);
  std::vector<uint8_t> data(size);
  file.read(reinterpret_cast<char*>(data.data()), size);
  return data;
}

// Helper to create hex dump
void hexdump(const std::vector<uint8_t>& data, size_t max_bytes = 64) {
  for (size_t i = 0; i < std::min(data.size(), max_bytes); ++i) {
    std::cout << std::hex << std::setw(2) << std::setfill('0') << static_cast<int>(data[i]) << " ";
    if ((i + 1) % 16 == 0)
      std::cout << "\n";
  }
  if (data.size() > max_bytes) {
    std::cout << "... (" << std::dec << (data.size() - max_bytes) << " more bytes)\n";
  }
  std::cout << std::dec << "\n";
}

void test_client_hello() {
  std::cout << "=== Test: Client Hello ===\n";

  auto data = read_file("captures/client_hello.bin");
  std::cout << "Read " << data.size() << " bytes\n";
  hexdump(data);

  std::string str_data(data.begin(), data.end());
  std::istringstream iss(str_data);
  kaitai::kstream ks(&iss);

  nix_daemon_protocol_t proto(PROTOCOL_VERSION_1_38, &ks);
  auto hello = std::make_unique<nix_daemon_protocol_t::client_hello_t>(&ks, nullptr, &proto);

  std::cout << "  Magic bytes: ";
  for (unsigned char c : hello->magic()) {
    std::cout << std::hex << std::setw(2) << std::setfill('0') << static_cast<int>(c) << " ";
  }
  std::cout << std::dec << "\n";
  assert(hello->magic() == WORKER_MAGIC_1_STR);

  uint16_t major = (hello->client_version() >> 8) & 0xff;
  uint16_t minor = hello->client_version() & 0xff;
  std::cout << "  Version: " << major << "." << minor << "\n";
  assert(major == 1 && minor == 38);

  std::cout << "  PASSED\n\n";
}

void test_server_hello() {
  std::cout << "=== Test: Server Hello ===\n";

  auto data = read_file("captures/server_hello.bin");
  std::cout << "Read " << data.size() << " bytes\n";
  hexdump(data);

  std::string str_data(data.begin(), data.end());
  std::istringstream iss(str_data);
  kaitai::kstream ks(&iss);

  nix_daemon_protocol_t proto(PROTOCOL_VERSION_1_38, &ks);
  auto hello = std::make_unique<nix_daemon_protocol_t::server_hello_t>(&ks, nullptr, &proto);

  std::cout << "  Magic bytes: ";
  for (unsigned char c : hello->magic()) {
    std::cout << std::hex << std::setw(2) << std::setfill('0') << static_cast<int>(c) << " ";
  }
  std::cout << std::dec << "\n";
  assert(hello->magic() == WORKER_MAGIC_2_STR);

  uint16_t major = (hello->server_version() >> 8) & 0xff;
  uint16_t minor = hello->server_version() & 0xff;
  std::cout << "  Version: " << major << "." << minor << "\n";
  assert(major == 1 && minor == 38);

  std::cout << "  PASSED\n\n";
}

void test_query_path_info_request() {
  std::cout << "=== Test: QueryPathInfo Request ===\n";

  auto data = read_file("captures/query_path_info_request.bin");
  std::cout << "Read " << data.size() << " bytes\n";
  hexdump(data);

  std::string str_data(data.begin(), data.end());
  std::istringstream iss(str_data);
  kaitai::kstream ks(&iss);

  nix_daemon_protocol_t proto(PROTOCOL_VERSION_1_38, &ks);
  auto request = std::make_unique<nix_daemon_protocol_t::request_t>(&ks, nullptr, &proto);

  std::cout << "  Op: " << static_cast<int>(request->op()) << "\n";
  assert(request->op() == nix_daemon_protocol_t::OPERATION_QUERY_PATH_INFO);

  auto* payload =
      dynamic_cast<nix_daemon_protocol_t::query_path_info_request_t*>(request->payload());
  assert(payload != nullptr);

  std::string path = payload->path()->path()->data();
  std::cout << "  Path: " << path << "\n";
  assert(path.find("/nix/store/") == 0);
  assert(path.find("hello") != std::string::npos);

  std::cout << "  PASSED\n\n";
}

void test_query_path_info_response() {
  std::cout << "=== Test: QueryPathInfo Response ===\n";

  auto data = read_file("captures/query_path_info.bin");
  std::cout << "Read " << data.size() << " bytes\n";
  hexdump(data);

  std::string str_data(data.begin(), data.end());
  std::istringstream iss(str_data);
  kaitai::kstream ks(&iss);

  nix_daemon_protocol_t proto(PROTOCOL_VERSION_1_38, &ks);
  auto response =
      std::make_unique<nix_daemon_protocol_t::query_path_info_response_t>(&ks, nullptr, &proto);

  std::cout << "  Valid: " << response->valid() << "\n";
  assert(response->valid() == 1);

  if (response->valid()) {
    auto* info = response->info();
    assert(info != nullptr);

    std::string deriver = info->deriver()->path()->data();
    std::cout << "  Deriver: " << deriver << "\n";
    assert(deriver.find(".drv") != std::string::npos);

    std::string hash = info->nar_hash()->data();
    std::cout << "  NAR hash: " << hash << "\n";
    assert(hash.length() == 64); // SHA256 hex

    uint64_t refs_count = info->references()->num_paths();
    std::cout << "  References: " << refs_count << "\n";

    std::cout << "  NAR size: " << info->nar_size() << "\n";
    assert(info->nar_size() > 0);

    std::cout << "  Ultimate: " << info->ultimate() << "\n";

    uint64_t sigs_count = info->sigs()->num_items();
    std::cout << "  Signatures: " << sigs_count << "\n";

    if (sigs_count > 0) {
      std::string first_sig = info->sigs()->items()->at(0)->data();
      std::cout << "    First sig: " << first_sig.substr(0, 40) << "...\n";
    }
  }

  std::cout << "  PASSED\n\n";
}

} // namespace

int main() {
  std::cout << "Nix Daemon Protocol Parser Test Suite\n";
  std::cout << "======================================\n\n";

  try {
    test_client_hello();
    test_server_hello();
    test_query_path_info_request();
    test_query_path_info_response();

    std::cout << "======================================\n";
    std::cout << "All tests passed!\n";
    return 0;
  } catch (const std::exception& e) {
    std::cerr << "FAILED: " << e.what() << "\n";
    return 1;
  }
}
