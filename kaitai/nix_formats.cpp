// kaitai/nix_formats.cpp
//
// Parser implementations for Cornell-generated Nix format specs.
// These functions implement the parsing logic declared in nix_formats.h.
//
// The serialization functions are all inline in the header (trivial).
// The parsing functions are more complex and implemented here.

#include "nix_formats.h"

#include <algorithm>
#include <charconv>
#include <sstream>

namespace cornell::nix {

// ═══════════════════════════════════════════════════════════════════════════════
// NAR PARSER
// ═══════════════════════════════════════════════════════════════════════════════

namespace {

struct nar_parser_state_t {
  std::span<const std::uint8_t> input;
  std::size_t depth = 0;

  auto expect_string(std::string_view expected) -> bool {
    auto result = parse_nix_string(input);
    if (!result.is_ok()) {
      return false;
    }
    if (result.value.value() != expected) {
      return false;
    }
    input = result.remaining;
    return true;
  }

  auto read_string() -> std::optional<std::string> {
    auto result = parse_nix_string(input);
    if (!result.is_ok()) {
      return std::nullopt;
    }
    input = result.remaining;
    return result.value;
  }

  auto read_contents() -> std::optional<std::vector<std::uint8_t>> {
    auto len_result = parse_u64le(input);
    if (!len_result.is_ok()) {
      return std::nullopt;
    }

    const auto len = static_cast<std::size_t>(len_result.value.value());
    if (len > max_file_size) {
      return std::nullopt;
    }

    const auto pad_len = pad_size(len);
    const auto total_len = len + pad_len;

    if (len_result.remaining.size() < total_len) {
      return std::nullopt;
    }

    std::vector<std::uint8_t> data(len_result.remaining.begin(),
                                   len_result.remaining.begin() + static_cast<std::ptrdiff_t>(len));
    input = len_result.remaining.subspan(total_len);
    return data;
  }

  auto parse_node() -> std::optional<nar_node_t>;
};

auto nar_parser_state_t::parse_node() -> std::optional<nar_node_t> {
  if (depth >= max_nar_depth) {
    return std::nullopt;
  }
  ++depth;

  if (!expect_string("(")) {
    return std::nullopt;
  }
  if (!expect_string("type")) {
    return std::nullopt;
  }

  auto node_type = read_string();
  if (!node_type) {
    return std::nullopt;
  }

  nar_node_t result;

  if (*node_type == "regular") {
    nar_node_t::file_t file;
    file.executable = false;

    // Check for executable or contents
    auto next = read_string();
    if (!next) {
      return std::nullopt;
    }

    if (*next == "executable") {
      file.executable = true;
      // Read empty string marker
      auto empty = read_string();
      if (!empty || !empty->empty()) {
        return std::nullopt;
      }
      // Read "contents"
      next = read_string();
      if (!next) {
        return std::nullopt;
      }
    }

    if (*next != "contents") {
      return std::nullopt;
    }

    auto contents = read_contents();
    if (!contents) {
      return std::nullopt;
    }
    file.contents = std::move(*contents);

    result.data = std::move(file);

  } else if (*node_type == "directory") {
    nar_node_t::dir_t dir;
    std::string prev_name;

    while (true) {
      auto token = read_string();
      if (!token) {
        return std::nullopt;
      }

      if (*token == ")") {
        // Oops, we read too far. Need to handle this differently.
        // The closing paren is for the directory node itself.
        // Put it back (can't really do this with spans, so restructure).
        // Actually, the directory entries end when we see ")", not "entry".
        // Let's check for "entry" or ")".
        break;
      }

      if (*token != "entry") {
        // It's ")". We're done with entries.
        // But we already consumed it. Need to handle this at outer level.
        // Actually, restructure: peek at token before consuming.
        // For now, if it's ")", we're done.
        if (*token == ")") {
          break;
        }
        return std::nullopt;
      }

      // Parse entry body
      if (!expect_string("(")) {
        return std::nullopt;
      }
      if (!expect_string("name")) {
        return std::nullopt;
      }

      auto entry_name = read_string();
      if (!entry_name) {
        return std::nullopt;
      }
      if (entry_name->size() > max_entry_name_len) {
        return std::nullopt;
      }

      // Enforce sorted order
      if (!prev_name.empty() && *entry_name <= prev_name) {
        return std::nullopt;
      }
      prev_name = *entry_name;

      if (!expect_string("node")) {
        return std::nullopt;
      }

      auto child = parse_node();
      if (!child) {
        return std::nullopt;
      }

      if (!expect_string(")")) {
        return std::nullopt;
      }

      nar_entry_t entry;
      entry.name = std::move(*entry_name);
      entry.node = std::make_unique<nar_node_t>(std::move(*child));
      dir.entries.push_back(std::move(entry));
    }

    result.data = std::move(dir);

    // Note: we already consumed the ")" in the loop above for directory
    --depth;
    return result;

  } else if (*node_type == "symlink") {
    if (!expect_string("target")) {
      return std::nullopt;
    }

    auto target = read_string();
    if (!target) {
      return std::nullopt;
    }

    nar_node_t::symlink_t link;
    link.target = std::move(*target);
    result.data = std::move(link);

  } else {
    return std::nullopt;
  }

  if (!expect_string(")")) {
    return std::nullopt;
  }

  --depth;
  return result;
}

} // namespace

auto parse_nar(std::span<const std::uint8_t> bs) -> parse_result_t<nar_t> {
  // Check magic
  auto magic_result = parse_nix_string(bs);
  if (!magic_result.is_ok()) {
    return parse_result_t<nar_t>::incomplete(bs);
  }
  if (magic_result.value.value() != nar_magic) {
    return parse_result_t<nar_t>::fail("invalid NAR magic");
  }

  nar_parser_state_t state;
  state.input = magic_result.remaining;

  auto root = state.parse_node();
  if (!root) {
    return parse_result_t<nar_t>::fail("failed to parse NAR root node");
  }

  nar_t nar;
  nar.root = std::move(*root);
  return parse_result_t<nar_t>::ok(std::move(nar), state.input);
}

// ═══════════════════════════════════════════════════════════════════════════════
// NARINFO PARSER
// ═══════════════════════════════════════════════════════════════════════════════

auto parse_narinfo(std::string_view text) -> parse_result_t<narinfo_t> {
  narinfo_t result;
  result.compression = compression_t::none;
  result.file_size = 0;
  result.nar_size = 0;

  bool has_store_path = false;
  bool has_url = false;
  bool has_nar_size = false;
  bool has_nar_hash = false;

  std::istringstream stream{std::string{text}};
  std::string line;

  while (std::getline(stream, line)) {
    if (line.empty()) {
      continue;
    }

    const auto colon_pos = line.find(':');
    if (colon_pos == std::string::npos) {
      return parse_result_t<narinfo_t>::fail("invalid narinfo line: " + line);
    }

    const auto key = line.substr(0, colon_pos);
    auto value = line.substr(colon_pos + 1);

    // Trim leading whitespace from value
    while (!value.empty() && (value[0] == ' ' || value[0] == '\t')) {
      value = value.substr(1);
    }

    if (key == "StorePath") {
      result.store_path = value;
      has_store_path = true;
    } else if (key == "URL") {
      result.url = value;
      has_url = true;
    } else if (key == "Compression") {
      auto comp = compression_from_string(value);
      if (!comp) {
        return parse_result_t<narinfo_t>::fail("invalid compression: " + value);
      }
      result.compression = *comp;
    } else if (key == "FileSize") {
      auto [ptr, ec] = std::from_chars(value.data(), value.data() + value.size(), result.file_size);
      if (ec != std::errc{}) {
        return parse_result_t<narinfo_t>::fail("invalid FileSize: " + value);
      }
    } else if (key == "FileHash") {
      result.file_hash = value;
    } else if (key == "NarSize") {
      auto [ptr, ec] = std::from_chars(value.data(), value.data() + value.size(), result.nar_size);
      if (ec != std::errc{}) {
        return parse_result_t<narinfo_t>::fail("invalid NarSize: " + value);
      }
      has_nar_size = true;
    } else if (key == "NarHash") {
      result.nar_hash = value;
      has_nar_hash = true;
    } else if (key == "References") {
      // Space-separated list
      std::istringstream refs(value);
      std::string ref;
      while (refs >> ref) {
        result.references.push_back(ref);
      }
    } else if (key == "Deriver") {
      result.deriver = value;
    } else if (key == "Sig") {
      // Format: keyname:base64sig
      const auto sig_colon = value.find(':');
      if (sig_colon == std::string::npos) {
        return parse_result_t<narinfo_t>::fail("invalid Sig format: " + value);
      }
      sig_t sig;
      sig.key_name = value.substr(0, sig_colon);
      sig.sig = value.substr(sig_colon + 1);
      result.sigs.push_back(std::move(sig));
    } else if (key == "CA") {
      result.ca = value;
    }
    // Unknown fields are ignored (forward compatibility)
  }

  // Check required fields
  if (!has_store_path) {
    return parse_result_t<narinfo_t>::fail("missing StorePath");
  }
  if (!has_url) {
    return parse_result_t<narinfo_t>::fail("missing URL");
  }
  if (!has_nar_size) {
    return parse_result_t<narinfo_t>::fail("missing NarSize");
  }
  if (!has_nar_hash) {
    return parse_result_t<narinfo_t>::fail("missing NarHash");
  }

  return parse_result_t<narinfo_t>::ok(std::move(result), {});
}

// ═══════════════════════════════════════════════════════════════════════════════
// DERIVATION PARSER
// ═══════════════════════════════════════════════════════════════════════════════

namespace {

struct drv_parser_state_t {
  std::string_view input;

  auto skip_whitespace() -> void {
    while (!input.empty() && (input[0] == ' ' || input[0] == '\t' || input[0] == '\n')) {
      input = input.substr(1);
    }
  }

  auto expect(char c) -> bool {
    skip_whitespace();
    if (input.empty() || input[0] != c) {
      return false;
    }
    input = input.substr(1);
    return true;
  }

  auto expect(std::string_view s) -> bool {
    skip_whitespace();
    if (!input.starts_with(s)) {
      return false;
    }
    input = input.substr(s.size());
    return true;
  }

  auto parse_quoted_string() -> std::optional<std::string> {
    skip_whitespace();
    if (input.empty() || input[0] != '"') {
      return std::nullopt;
    }
    input = input.substr(1);

    std::string result;
    while (!input.empty() && input[0] != '"') {
      if (input[0] == '\\' && input.size() > 1) {
        switch (input[1]) {
          case 'n':
            result += '\n';
            break;
          case 'r':
            result += '\r';
            break;
          case 't':
            result += '\t';
            break;
          case '\\':
            result += '\\';
            break;
          case '"':
            result += '"';
            break;
          default:
            result += input[1];
            break;
        }
        input = input.substr(2);
      } else {
        result += input[0];
        input = input.substr(1);
      }
    }

    if (input.empty() || input[0] != '"') {
      return std::nullopt;
    }
    input = input.substr(1);
    return result;
  }

  auto parse_string_list() -> std::optional<std::vector<std::string>> {
    if (!expect('[')) {
      return std::nullopt;
    }

    std::vector<std::string> result;
    skip_whitespace();

    if (!input.empty() && input[0] == ']') {
      input = input.substr(1);
      return result;
    }

    while (true) {
      auto s = parse_quoted_string();
      if (!s) {
        return std::nullopt;
      }
      result.push_back(std::move(*s));

      skip_whitespace();
      if (input.empty()) {
        return std::nullopt;
      }

      if (input[0] == ']') {
        input = input.substr(1);
        return result;
      }

      if (input[0] != ',') {
        return std::nullopt;
      }
      input = input.substr(1);
    }
  }
};

} // namespace

auto parse_derivation(std::string_view text) -> parse_result_t<derivation_t> {
  drv_parser_state_t state;
  state.input = text;

  if (!state.expect("Derive(")) {
    return parse_result_t<derivation_t>::fail("expected 'Derive('");
  }

  derivation_t drv;

  // Parse outputs: [("name","path","hashAlgo","hash"),...]
  if (!state.expect('[')) {
    return parse_result_t<derivation_t>::fail("expected '[' for outputs");
  }

  state.skip_whitespace();
  while (!state.input.empty() && state.input[0] != ']') {
    if (!state.expect('(')) {
      return parse_result_t<derivation_t>::fail("expected '(' for output");
    }

    drv_output_t output;
    auto name = state.parse_quoted_string();
    if (!name) {
      return parse_result_t<derivation_t>::fail("expected output name");
    }
    output.name = std::move(*name);

    if (!state.expect(',')) {
      return parse_result_t<derivation_t>::fail("expected ','");
    }

    auto path = state.parse_quoted_string();
    if (!path) {
      return parse_result_t<derivation_t>::fail("expected output path");
    }
    output.path = std::move(*path);

    if (!state.expect(',')) {
      return parse_result_t<derivation_t>::fail("expected ','");
    }

    auto hash_algo = state.parse_quoted_string();
    if (!hash_algo) {
      return parse_result_t<derivation_t>::fail("expected hash algo");
    }
    output.hash_algo = std::move(*hash_algo);

    if (!state.expect(',')) {
      return parse_result_t<derivation_t>::fail("expected ','");
    }

    auto hash = state.parse_quoted_string();
    if (!hash) {
      return parse_result_t<derivation_t>::fail("expected hash");
    }
    output.hash = std::move(*hash);

    if (!state.expect(')')) {
      return parse_result_t<derivation_t>::fail("expected ')'");
    }

    drv.outputs.push_back(std::move(output));

    state.skip_whitespace();
    if (!state.input.empty() && state.input[0] == ',') {
      state.input = state.input.substr(1);
    }
    state.skip_whitespace();
  }

  if (!state.expect(']')) {
    return parse_result_t<derivation_t>::fail("expected ']'");
  }
  if (!state.expect(',')) {
    return parse_result_t<derivation_t>::fail("expected ','");
  }

  // Parse input derivations: [("path",["out"]),...]
  if (!state.expect('[')) {
    return parse_result_t<derivation_t>::fail("expected '[' for inputDrvs");
  }

  state.skip_whitespace();
  while (!state.input.empty() && state.input[0] != ']') {
    if (!state.expect('(')) {
      return parse_result_t<derivation_t>::fail("expected '(' for inputDrv");
    }

    drv_input_t drv_input;
    auto drv_path = state.parse_quoted_string();
    if (!drv_path) {
      return parse_result_t<derivation_t>::fail("expected drv path");
    }
    drv_input.drv_path = std::move(*drv_path);

    if (!state.expect(',')) {
      return parse_result_t<derivation_t>::fail("expected ','");
    }

    auto outputs = state.parse_string_list();
    if (!outputs) {
      return parse_result_t<derivation_t>::fail("expected output list");
    }
    drv_input.output_names = std::move(*outputs);

    if (!state.expect(')')) {
      return parse_result_t<derivation_t>::fail("expected ')'");
    }

    drv.input_drvs.push_back(std::move(drv_input));

    state.skip_whitespace();
    if (!state.input.empty() && state.input[0] == ',') {
      state.input = state.input.substr(1);
    }
    state.skip_whitespace();
  }

  if (!state.expect(']')) {
    return parse_result_t<derivation_t>::fail("expected ']'");
  }
  if (!state.expect(',')) {
    return parse_result_t<derivation_t>::fail("expected ','");
  }

  // Parse input sources
  auto input_srcs = state.parse_string_list();
  if (!input_srcs) {
    return parse_result_t<derivation_t>::fail("expected inputSrcs");
  }
  drv.input_srcs = std::move(*input_srcs);

  if (!state.expect(',')) {
    return parse_result_t<derivation_t>::fail("expected ','");
  }

  // Platform
  auto platform = state.parse_quoted_string();
  if (!platform) {
    return parse_result_t<derivation_t>::fail("expected platform");
  }
  drv.platform = std::move(*platform);

  if (!state.expect(',')) {
    return parse_result_t<derivation_t>::fail("expected ','");
  }

  // Builder
  auto builder = state.parse_quoted_string();
  if (!builder) {
    return parse_result_t<derivation_t>::fail("expected builder");
  }
  drv.builder = std::move(*builder);

  if (!state.expect(',')) {
    return parse_result_t<derivation_t>::fail("expected ','");
  }

  // Args
  auto args = state.parse_string_list();
  if (!args) {
    return parse_result_t<derivation_t>::fail("expected args");
  }
  drv.args = std::move(*args);

  if (!state.expect(',')) {
    return parse_result_t<derivation_t>::fail("expected ','");
  }

  // Environment: [("key","value"),...]
  if (!state.expect('[')) {
    return parse_result_t<derivation_t>::fail("expected '[' for env");
  }

  state.skip_whitespace();
  while (!state.input.empty() && state.input[0] != ']') {
    if (!state.expect('(')) {
      return parse_result_t<derivation_t>::fail("expected '(' for env pair");
    }

    auto key = state.parse_quoted_string();
    if (!key) {
      return parse_result_t<derivation_t>::fail("expected env key");
    }

    if (!state.expect(',')) {
      return parse_result_t<derivation_t>::fail("expected ','");
    }

    auto value = state.parse_quoted_string();
    if (!value) {
      return parse_result_t<derivation_t>::fail("expected env value");
    }

    if (!state.expect(')')) {
      return parse_result_t<derivation_t>::fail("expected ')'");
    }

    drv.env.emplace_back(std::move(*key), std::move(*value));

    state.skip_whitespace();
    if (!state.input.empty() && state.input[0] == ',') {
      state.input = state.input.substr(1);
    }
    state.skip_whitespace();
  }

  if (!state.expect(']')) {
    return parse_result_t<derivation_t>::fail("expected ']'");
  }
  if (!state.expect(')')) {
    return parse_result_t<derivation_t>::fail("expected ')'");
  }

  return parse_result_t<derivation_t>::ok(std::move(drv), {});
}

} // namespace cornell::nix
