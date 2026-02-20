#include "nix/store/derivations.h"

#include <optional>

#include <boost/container/small_vector.hpp>
#include <boost/unordered/concurrent_flat_map.hpp>
#include <nlohmann/json.hpp>

#include "nix/store/async-path-writer.h"
#include "nix/store/common-protocol-impl.h"
#include "nix/store/common-protocol.h"
#include "nix/store/downstream-placeholder.h"
#include "nix/store/globals.h"
#include "nix/store/store-api.h"
#include "nix/util/json-utils.h"
#include "nix/util/split.h"
#include "nix/util/strings-inline.h"
#include "nix/util/types.h"
#include "nix/util/util.h"

namespace nix {

using namespace std::literals::string_view_literals;

std::optional<store_path_t> derivation_output_t::path(const store_dir_config_t& store,
                                                std::string_view drv_name,
                                                OutputNameView output_name) const {
  return std::visit(
      overloaded{
          [](const derivation_output_t::InputAddressed& doi) -> std::optional<store_path_t> {
            return {doi.path};
          },
          [&](const derivation_output_t::CAFixed& dof) -> std::optional<store_path_t> {
            return {dof.path(store, drv_name, output_name)};
          },
          [](const derivation_output_t::CAFloating& dof) -> std::optional<store_path_t> {
            return std::nullopt;
          },
          [](const derivation_output_t::Deferred&) -> std::optional<store_path_t> {
            return std::nullopt;
          },
          [](const derivation_output_t::Impure&) -> std::optional<store_path_t> { return std::nullopt; },
      },
      raw);
}

store_path_t derivation_output_t::CAFixed::path(const store_dir_config_t& store, std::string_view drv_name,
                                          OutputNameView output_name) const {
  return store.makeFixedOutputPathFromCA(output_path_name(drv_name, output_name),
                                         ContentAddressWithReferences::withoutRefs(ca));
}

bool DerivationType::isCA() const {
  /* Normally we do the full `std::visit` to make sure we have
     exhaustively handled all variants, but so long as there is a
     variant called `ContentAddressed`, it must be the only one for
     which `isCA` is true for this to make sense!. */
  return std::visit(overloaded{
                        [](const InputAddressed& ia) { return false; },
                        [](const ContentAddressed& ca) { return true; },
                        [](const Impure&) { return true; },
                    },
                    raw);
}

bool DerivationType::isFixed() const {
  return std::visit(overloaded{
                        [](const InputAddressed& ia) { return false; },
                        [](const ContentAddressed& ca) { return ca.fixed; },
                        [](const Impure&) { return false; },
                    },
                    raw);
}

bool DerivationType::hasKnownOutputPaths() const {
  return std::visit(overloaded{
                        [](const InputAddressed& ia) { return !ia.deferred; },
                        [](const ContentAddressed& ca) { return ca.fixed; },
                        [](const Impure&) { return false; },
                    },
                    raw);
}

bool DerivationType::isSandboxed() const {
  return std::visit(overloaded{
                        [](const InputAddressed& ia) { return true; },
                        [](const ContentAddressed& ca) { return ca.sandboxed; },
                        [](const Impure&) { return false; },
                    },
                    raw);
}

bool DerivationType::is_impure() const {
  return std::visit(overloaded{
                        [](const InputAddressed& ia) { return false; },
                        [](const ContentAddressed& ca) { return false; },
                        [](const Impure&) { return true; },
                    },
                    raw);
}

bool basic_derivation_t::isBuiltin() const {
  return builder.substr(0, 8) == "builtin:";
}

static auto info_for_derivation(store_t& store, const derivation_t& drv) {
  auto references = drv.input_srcs;
  for (auto& i : drv.input_drvs.map)
    references.insert(i.first);
  /* Note that the outputs of a derivation are *not* references
     (that can be missing (of course) and should not necessarily be
     held during a garbage collection). */
  auto suffix = std::string(drv.name) + drvExtension;
  auto contents = drv.unparse(store, false);
  auto hash = hash_string(hash_algorithm_t::SHA256, contents);
  auto ca = TextInfo{.hash = hash, .references = references};
  return std::tuple{
      suffix,
      contents,
      references,
      store.makeFixedOutputPathFromCA(suffix, ca),
  };
}

store_path_t write_derivation(store_t& store, const derivation_t& drv, RepairFlag repair, bool read_only) {
  if (read_only || settings.readOnlyMode) {
    auto [_x, _y, _z, path] = info_for_derivation(store, drv);
    return path;
  } else
    return store.write_derivation(drv, repair);
}

store_path_t store_t::write_derivation(const derivation_t& drv, RepairFlag repair) {
  auto [suffix, contents, references, path] = info_for_derivation(*this, drv);

  if (isValidPath(path) && !repair)
    return path;

  string_source_t s{contents};
  auto path2 =
      add_to_store_from_dump(s, suffix, file_serialisation_method_t::flat, content_address_method_t::raw_t::Text,
                         hash_algorithm_t::SHA256, references, repair);
  assert(path2 == path);

  return path;
}

store_path_t write_derivation(store_t& store, AsyncPathWriter& async_path_writer, const derivation_t& drv,
                          RepairFlag repair, bool read_only) {
  auto references = drv.input_srcs;
  for (auto& i : drv.input_drvs.map)
    references.insert(i.first);
  return async_path_writer.add_path(drv.unparse(store, false), std::string(drv.name) + drvExtension,
                                 references, repair, read_only || settings.readOnlyMode);
}

namespace {
/**
 * This mimics std::istream to some extent. We use this much smaller implementation
 * instead of plain istreams because the sentry object overhead is too high.
 */
struct string_view_stream_t {
  std::string_view remaining;

  int peek() const { return remaining.empty() ? EOF : remaining[0]; }

  int get() {
    if (remaining.empty())
      return EOF;
    char c = remaining[0];
    remaining.remove_prefix(1);
    return c;
  }
};

constexpr struct escapes_t {
  char map[256];

  constexpr escapes_t() {
    for (int i = 0; i < 256; i++)
      map[i] = (char)(unsigned char)i;
    map[(int)(unsigned char)'n'] = '\n';
    map[(int)(unsigned char)'r'] = '\r';
    map[(int)(unsigned char)'t'] = '\t';
  }

  char operator[](char c) const { return map[(unsigned char)c]; }
} escapes;
} // namespace

/* Read string `s' from stream `str'. */
static void expect(string_view_stream_t& str, std::string_view s) {
  if (!str.remaining.starts_with(s))
    throw FormatError("expected string '%1%'", s);
  str.remaining.remove_prefix(s.size());
}

static void expect(string_view_stream_t& str, char c) {
  if (str.remaining.empty() || str.remaining[0] != c)
    throw FormatError("expected string '%1%'", c);
  str.remaining.remove_prefix(1);
}

/* Read a C-style string from stream `str'. */
static backed_string_view_t parse_string(string_view_stream_t& str) {
  expect(str, '"');
  size_t start = 0;
  size_t end = str.remaining.size();
  const auto data = str.remaining.data();
  while (start < end) {
    auto idx = str.remaining.find('"', start);
    if (idx == std::string_view::npos) {
      break;
    }
    size_t pos = idx;
    for (; pos > 0 && data[pos - 1] == '\\'; pos--)
      ;
    if ((idx - pos) % 2 == 0) { // even number of backslashes
      end = idx;
      break;
    }
    start = idx + 1;
  }

  start = 0;
  const auto content = str.remaining.substr(start, end);
  str.remaining.remove_prefix(end + 1);

  auto next_backslash = content.find('\\', start);
  if (next_backslash == std::string_view::npos) {
    return content;
  }

  std::string res;
  res.reserve(end);
  do {
    if (next_backslash == end - 1) {
      throw FormatError("unterminated string in derivation");
    }
    if (next_backslash > start) {
      res.append(&data[start], next_backslash - start);
    }
    res.push_back(escapes[data[next_backslash + 1]]);
    start = next_backslash + 2;
    next_backslash = content.find('\\', start);
  } while (next_backslash != std::string_view::npos);
  if (end > start) {
    res.append(&data[start], end - start);
  }
  return res;
}

static void validate_path(std::string_view s) {
  if (s.size() == 0 || s[0] != '/')
    throw FormatError("bad path '%1%' in derivation", s);
}

static backed_string_view_t parse_path(string_view_stream_t& str) {
  auto s = parse_string(str);
  validate_path(*s);
  return s;
}

static bool end_of_list(string_view_stream_t& str) {
  if (str.peek() == ',') {
    str.get();
    return false;
  }
  if (str.peek() == ']') {
    str.get();
    return true;
  }
  return false;
}

static string_set_t parse_strings(string_view_stream_t& str, bool are_paths) {
  string_set_t res;
  expect(str, '[');
  while (!end_of_list(str))
    res.insert((are_paths ? parse_path(str) : parse_string(str)).to_owned());
  return res;
}

static derivation_output_t parse_derivation_output(const store_dir_config_t& store, std::string_view path_s,
                                              std::string_view hash_algo_str, std::string_view hash_s,
                                              const experimental_feature_settings_t& xp_settings) {
  if (!hash_algo_str.empty()) {
    content_address_method_t method = content_address_method_t::parsePrefix(hash_algo_str);
    if (method == content_address_method_t::raw_t::Text)
      xp_settings.require(xp_t::dynamic_derivations, "text-hashed derivation output");
    const auto hash_algo = parse_hash_algo(hash_algo_str);
    if (hash_s == "impure"sv) {
      xp_settings.require(xp_t::impure_derivations);
      if (!path_s.empty())
        throw FormatError("impure derivation output should not specify output path");
      return derivation_output_t::Impure{
          .method = std::move(method),
          .hash_algo = std::move(hash_algo),
      };
    } else if (!hash_s.empty()) {
      validate_path(path_s);
      auto hash = Hash::parse_non_sri_unprefixed(hash_s, hash_algo);
      return derivation_output_t::CAFixed{
          .ca =
              content_address_t{
                  .method = std::move(method),
                  .hash = std::move(hash),
              },
      };
    } else {
      xp_settings.require(xp_t::ca_derivations);
      if (!path_s.empty())
        throw FormatError("content-addressing derivation output should not specify output path");
      return derivation_output_t::CAFloating{
          .method = std::move(method),
          .hash_algo = std::move(hash_algo),
      };
    }
  } else {
    if (path_s.empty()) {
      return derivation_output_t::Deferred{};
    }
    validate_path(path_s);
    return derivation_output_t::InputAddressed{
        .path = store.parseStorePath(path_s),
    };
  }
}

static derivation_output_t
parse_derivation_output(const store_dir_config_t& store, string_view_stream_t& str,
                      const experimental_feature_settings_t& xp_settings = experimental_feature_settings) {
  expect(str, ',');
  const auto path_s = parse_string(str);
  expect(str, ',');
  const auto hash_algo = parse_string(str);
  expect(str, ',');
  const auto hash = parse_string(str);
  expect(str, ')');

  return parse_derivation_output(store, *path_s, *hash_algo, *hash, xp_settings);
}

/**
 * All ATerm derivation_t format versions currently known.
 *
 * Unknown versions are rejected at the parsing stage.
 */
enum struct derivation_a_term_version_t {
  /**
   * Older unversioned form
   */
  traditional,

  /**
   * Newer versioned form; only this version so far.
   */
  dynamic_derivations,
};

static DerivedPathMap<string_set_t>::ChildNode
parse_derived_path_map_node(const store_dir_config_t& store, string_view_stream_t& str,
                        derivation_a_term_version_t version) {
  DerivedPathMap<string_set_t>::ChildNode node;

  auto parse_non_dynamic = [&]() { node.value = parse_strings(str, false); };

  // Older derivation should never use new form, but newer
  // derivaiton can use old form.
  switch (version) {
    case derivation_a_term_version_t::traditional:
      parse_non_dynamic();
      break;
    case derivation_a_term_version_t::dynamic_derivations:
      switch (str.peek()) {
        case '[':
          parse_non_dynamic();
          break;
        case '(':
          expect(str, '(');
          node.value = parse_strings(str, false);
          expect(str, ",["sv);
          while (!end_of_list(str)) {
            expect(str, '(');
            auto output_name = parse_string(str).to_owned();
            expect(str, ',');
            node.childMap.insert_or_assign(output_name,
                                           parse_derived_path_map_node(store, str, version));
            expect(str, ')');
          }
          expect(str, ')');
          break;
        default:
          throw FormatError("invalid inputDrvs entry in derivation");
      }
      break;
    default:
      // invalid format, not a parse error but internal error
      assert(false);
  }
  return node;
}

derivation_t parse_derivation(const store_dir_config_t& store, std::string&& s, std::string_view name,
                           const experimental_feature_settings_t& xp_settings) {
  derivation_t drv;
  drv.name = name;

  string_view_stream_t str{s};
  expect(str, 'D');
  derivation_a_term_version_t version;
  switch (str.peek()) {
    case 'e':
      expect(str, "erive("sv);
      version = derivation_a_term_version_t::traditional;
      break;
    case 'r': {
      expect(str, "rvWithVersion("sv);
      auto version_s = parse_string(str);
      if (*version_s == "xp-dyn-drv"sv) {
        // Only version we have so far
        version = derivation_a_term_version_t::dynamic_derivations;
        xp_settings.require(xp_t::dynamic_derivations, [&] {
          return fmt("derivation '%s', ATerm format version 'xp-dyn-drv'", name);
        });
      } else {
        throw FormatError("Unknown derivation ATerm format version '%s'", *version_s);
      }
      expect(str, ',');
      break;
    }
    default:
      throw Error("derivation does not start with 'Derive' or 'DrvWithVersion'");
  }

  /* Parse the list of outputs. */
  expect(str, '[');
  while (!end_of_list(str)) {
    expect(str, '(');
    std::string id = parse_string(str).to_owned();
    auto output = parse_derivation_output(store, str, xp_settings);
    drv.outputs.emplace(std::move(id), std::move(output));
  }

  /* Parse the list of input derivations. */
  expect(str, ",["sv);
  while (!end_of_list(str)) {
    expect(str, '(');
    auto drv_path = parse_path(str);
    expect(str, ',');
    drv.input_drvs.map.insert_or_assign(store.parseStorePath(*drv_path),
                                       parse_derived_path_map_node(store, str, version));
    expect(str, ')');
  }

  expect(str, ',');
  drv.input_srcs = store.parseStorePathSet(parse_strings(str, true));
  expect(str, ',');
  drv.platform = parse_string(str).to_owned();
  expect(str, ',');
  drv.builder = parse_string(str).to_owned();

  /* Parse the builder arguments. */
  expect(str, ",["sv);
  while (!end_of_list(str))
    drv.args.push_back(parse_string(str).to_owned());

  /* Parse the environment variables. */
  expect(str, ",["sv);
  while (!end_of_list(str)) {
    expect(str, '(');
    auto name = parse_string(str).to_owned();
    expect(str, ',');
    auto value = parse_string(str);
    if (name == StructuredAttrs::envVarName) {
      drv.structured_attrs = StructuredAttrs::parse(*std::move(value));
    } else {
      drv.env.insert_or_assign(std::move(name), std::move(value).to_owned());
    }
    expect(str, ')');
  }

  expect(str, ')');
  return drv;
}

/**
 * Print a derivation string literal to an `std::string`.
 *
 * This syntax does not generalize to the expression language, which needs to
 * escape `$`.
 *
 * @param res Where to print to
 * @param s Which logical string to print
 */
static void print_string(std::string& res, std::string_view s) {
  res.reserve(res.size() + s.size() * 2 + 2);
  res += '"';
  static constexpr auto chunk_size = 1024;
  std::array<char, 2 * chunk_size + 2> buffer;
  while (!s.empty()) {
    auto chunk = s.substr(0, /*n=*/chunk_size);
    s.remove_prefix(chunk.size());
    char* buf = buffer.data();
    char* p = buf;
    for (auto c : chunk)
      if (c == '\"' || c == '\\') {
        *p++ = '\\';
        *p++ = c;
      } else if (c == '\n') {
        *p++ = '\\';
        *p++ = 'n';
      } else if (c == '\r') {
        *p++ = '\\';
        *p++ = 'r';
      } else if (c == '\t') {
        *p++ = '\\';
        *p++ = 't';
      } else
        *p++ = c;
    res.append(buf, p - buf);
  }
  res += '"';
}

static void print_unquoted_string(std::string& res, std::string_view s) {
  res += '"';
  res.append(s);
  res += '"';
}

template <class ForwardIterator>
static void print_strings(std::string& res, ForwardIterator i, ForwardIterator j) {
  res += '[';
  bool first = true;
  for (; i != j; ++i) {
    if (first)
      first = false;
    else
      res += ',';
    print_string(res, *i);
  }
  res += ']';
}

template <class ForwardIterator>
static void print_unquoted_strings(std::string& res, ForwardIterator i, ForwardIterator j) {
  res += '[';
  bool first = true;
  for (; i != j; ++i) {
    if (first)
      first = false;
    else
      res += ',';
    print_unquoted_string(res, *i);
  }
  res += ']';
}

static void unparse_derived_path_map_node(const store_dir_config_t& store, std::string& s,
                                      const DerivedPathMap<string_set_t>::ChildNode& node) {
  s += ',';
  if (node.childMap.empty()) {
    print_unquoted_strings(s, node.value.begin(), node.value.end());
  } else {
    s += '(';
    print_unquoted_strings(s, node.value.begin(), node.value.end());
    s += ",["sv;
    bool first = true;
    for (auto& [output_name, childNode] : node.childMap) {
      if (first)
        first = false;
      else
        s += ',';
      s += '(';
      print_unquoted_string(s, output_name);
      unparse_derived_path_map_node(store, s, childNode);
      s += ')';
    }
    s += "])"sv;
  }
}

/**
 * Does the derivation have a dependency on the output of a dynamic
 * derivation?
 *
 * In other words, does it on the output of derivation that is itself an
 * output of a derivation? This corresponds to a dependency that is an
 * inductive derived path with more than one layer of
 * `derived_path_t::Built`.
 */
static bool has_dynamic_drv_dep(const derivation_t& drv) {
  return std::find_if(drv.input_drvs.map.begin(), drv.input_drvs.map.end(), [](auto& kv) {
           return !kv.second.childMap.empty();
         }) != drv.input_drvs.map.end();
}

std::string derivation_t::unparse(const store_dir_config_t& store, bool mask_outputs,
                                DerivedPathMap<string_set_t>::ChildNode::Map* actualInputs) const {
  std::string s;
  s.reserve(65536);

  /* use older unversioned form if possible, for wider compat. use
     newer form only if we need it, which we do for
     `xp_t::dynamic_derivations`. */
  if (has_dynamic_drv_dep(*this)) {
    s += "DrvWithVersion("sv;
    // Only version we have so far
    print_unquoted_string(s, "xp-dyn-drv"sv);
    s += ',';
  } else {
    s += "Derive("sv;
  }

  bool first = true;
  s += '[';
  for (auto& i : outputs) {
    if (first)
      first = false;
    else
      s += ',';
    s += '(';
    print_unquoted_string(s, i.first);
    std::visit(
        overloaded{[&](const derivation_output_t::InputAddressed& doi) {
                     s += ',';
                     print_unquoted_string(s, mask_outputs ? ""sv : store.printStorePath(doi.path));
                     s += ',';
                     print_unquoted_string(s, {});
                     s += ',';
                     print_unquoted_string(s, {});
                   },
                   [&](const derivation_output_t::CAFixed& dof) {
                     s += ',';
                     print_unquoted_string(
                         s,
                         mask_outputs ? ""sv : store.printStorePath(dof.path(store, name, i.first)));
                     s += ',';
                     print_unquoted_string(s, dof.ca.printMethodAlgo());
                     s += ',';
                     print_unquoted_string(s, dof.ca.hash.to_string(hash_format_t::base16, false));
                   },
                   [&](const derivation_output_t::CAFloating& dof) {
                     s += ',';
                     print_unquoted_string(s, {});
                     s += ',';
                     print_unquoted_string(s, std::string{dof.method.renderPrefix()} +
                                                print_hash_algo(dof.hash_algo));
                     s += ',';
                     print_unquoted_string(s, {});
                   },
                   [&](const derivation_output_t::Deferred&) {
                     s += ',';
                     print_unquoted_string(s, {});
                     s += ',';
                     print_unquoted_string(s, {});
                     s += ',';
                     print_unquoted_string(s, {});
                   },
                   [&](const derivation_output_t::Impure& doi) {
                     // FIXME
                     s += ',';
                     print_unquoted_string(s, {});
                     s += ',';
                     print_unquoted_string(s, std::string{doi.method.renderPrefix()} +
                                                print_hash_algo(doi.hash_algo));
                     s += ',';
                     print_unquoted_string(s, "impure"sv);
                   }},
        i.second.raw);
    s += ')';
  }

  s += "],["sv;
  first = true;
  if (actualInputs) {
    for (auto& [drvHashModulo, childMap] : *actualInputs) {
      if (first)
        first = false;
      else
        s += ',';
      s += '(';
      print_unquoted_string(s, drvHashModulo);
      unparse_derived_path_map_node(store, s, childMap);
      s += ')';
    }
  } else {
    for (auto& [drv_path, childMap] : input_drvs.map) {
      if (first)
        first = false;
      else
        s += ',';
      s += '(';
      print_unquoted_string(s, store.printStorePath(drv_path));
      unparse_derived_path_map_node(store, s, childMap);
      s += ')';
    }
  }

  s += "],"sv;
  auto paths = store.printStorePathSet(input_srcs); // FIXME: slow
  print_unquoted_strings(s, paths.begin(), paths.end());

  s += ',';
  print_unquoted_string(s, platform);
  s += ',';
  print_string(s, builder);
  s += ',';
  print_strings(s, args.begin(), args.end());

  s += ",["sv;
  first = true;

  auto unparseEnv = [&](const string_pairs_t aterm_env) {
    for (auto& i : aterm_env) {
      if (first)
        first = false;
      else
        s += ',';
      s += '(';
      print_string(s, i.first);
      s += ',';
      print_string(s, mask_outputs && outputs.count(i.first) ? ""sv : i.second);
      s += ')';
    }
  };

  StructuredAttrs::checkKeyNotInUse(env);
  if (structured_attrs) {
    string_pairs_t scratch = env;
    scratch.insert(structured_attrs->unparse());
    unparseEnv(scratch);
  } else {
    unparseEnv(env);
  }

  s += "])"sv;

  return s;
}

// FIXME: remove
bool is_derivation(std::string_view file_name) {
  return has_suffix(file_name, drvExtension);
}

std::string output_path_name(std::string_view drv_name, OutputNameView output_name) {
  std::string res{drv_name};
  if (output_name != "out"sv) {
    res += '-';
    res += output_name;
  }
  return res;
}

DerivationType basic_derivation_t::type() const {
  std::optional<hash_algorithm_t> floatingHashAlgo;
  std::optional<DerivationType> ty;

  auto decide = [&](DerivationType newTy) {
    if (!ty)
      ty = newTy;
    else if (ty.value() != newTy)
      throw Error("can't mix derivation output types");
    else if (ty.value() == DerivationType::ContentAddressed{.sandboxed = false, .fixed = true})
      // FIXME: Experimental feature?
      throw Error("only one fixed output is allowed for now");
  };

  for (auto& i : outputs) {
    std::visit(overloaded{
                   [&](const derivation_output_t::InputAddressed&) {
                     decide(DerivationType::InputAddressed{
                         .deferred = false,
                     });
                   },
                   [&](const derivation_output_t::CAFixed&) {
                     decide(DerivationType::ContentAddressed{
                         .sandboxed = false,
                         .fixed = true,
                     });
                     if (i.first != "out"sv)
                       throw Error("single fixed output must be named \"out\"");
                   },
                   [&](const derivation_output_t::CAFloating& dof) {
                     decide(DerivationType::ContentAddressed{
                         .sandboxed = true,
                         .fixed = false,
                     });
                     if (!floatingHashAlgo)
                       floatingHashAlgo = dof.hash_algo;
                     else if (*floatingHashAlgo != dof.hash_algo)
                       throw Error("all floating outputs must use the same hash algorithm");
                   },
                   [&](const derivation_output_t::Deferred&) {
                     decide(DerivationType::InputAddressed{
                         .deferred = true,
                     });
                   },
                   [&](const derivation_output_t::Impure&) { decide(DerivationType::Impure{}); },
               },
               i.second.raw);
  }

  if (!ty)
    throw Error("must have at least one output");

  return ty.value();
}

DrvHashes drv_hashes;

/* path_derivation_modulo and hash_derivation_modulo are mutually recursive
 */

/* Look up the derivation by value and memoize the
   `hash_derivation_modulo` call.
 */
static const DrvHash path_derivation_modulo(store_t& store, const store_path_t& drv_path) {
  std::optional<DrvHash> hash;
  if (drv_hashes.cvisit(drv_path, [&hash](const auto& kv) { hash.emplace(kv.second); })) {
    return *hash;
  }
  auto h = hash_derivation_modulo(store, store.readInvalidDerivation(drv_path), false);
  // Cache it
  drv_hashes.insert_or_assign(drv_path, h);
  return h;
}

/* See the header for interface details. These are the implementation details.

   For fixed-output derivations, each hash in the map is not the
   corresponding output's content hash, but a hash of that hash along
   with other constant data. The key point is that the value is a pure
   function of the output's contents, and there are no preimage attacks
   either spoofing an output's contents for a derivation, or
   spoofing a derivation for an output's contents.

   For regular derivations, it looks up each subderivation from its hash
   and recurs. If the subderivation is also regular, it simply
   substitutes the derivation path with its hash. If the subderivation
   is fixed-output, however, it takes each output hash and pretends it
   is a derivation hash producing a single "out" output. This is so we
   don't leak the provenance of fixed outputs, reducing pointless cache
   misses as the build itself won't know this.
 */
DrvHash hash_derivation_modulo(store_t& store, const derivation_t& drv, bool mask_outputs) {
  auto type = drv.type();

  /* Return a fixed hash for fixed-output derivations. */
  if (type.isFixed()) {
    std::map<std::string, Hash> output_hashes;
    for (const auto& i : drv.outputs) {
      auto& dof = std::get<derivation_output_t::CAFixed>(i.second.raw);
      auto hash = hash_string(hash_algorithm_t::SHA256,
                             "fixed:out:" + dof.ca.printMethodAlgo() + ":" +
                                 dof.ca.hash.to_string(hash_format_t::base16, false) + ":" +
                                 store.printStorePath(dof.path(store, drv.name, i.first)));
      output_hashes.insert_or_assign(i.first, std::move(hash));
    }
    return DrvHash{
        .hashes = output_hashes,
        .kind = DrvHash::Kind::regular,
    };
  }

  auto kind =
      std::visit(overloaded{[](const DerivationType::InputAddressed& ia) {
                              /* This might be a "pesimistically" deferred output, so we don't
                                 "taint" the kind yet. */
                              return DrvHash::Kind::regular;
                            },
                            [](const DerivationType::ContentAddressed& ca) {
                              return ca.fixed ? DrvHash::Kind::regular : DrvHash::Kind::Deferred;
                            },
                            [](const DerivationType::Impure&) -> DrvHash::Kind {
                              return DrvHash::Kind::Deferred;
                            }},
                 drv.type().raw);

  DerivedPathMap<string_set_t>::ChildNode::Map inputs2;
  for (auto& [drv_path, node] : drv.input_drvs.map) {
    const auto& res = path_derivation_modulo(store, drv_path);
    if (res.kind == DrvHash::Kind::Deferred)
      kind = DrvHash::Kind::Deferred;
    for (auto& output_name : node.value) {
      const auto h = get(res.hashes, output_name);
      if (!h)
        throw Error("no hash for output '%s' of derivation '%s'", output_name, drv.name);
      inputs2[h->to_string(hash_format_t::base16, false)].value.insert(output_name);
    }
  }

  auto hash = hash_string(hash_algorithm_t::SHA256, drv.unparse(store, mask_outputs, &inputs2));

  std::map<std::string, Hash> output_hashes;
  for (const auto& [output_name, _] : drv.outputs) {
    output_hashes.insert_or_assign(output_name, hash);
  }

  return DrvHash{
      .hashes = output_hashes,
      .kind = kind,
  };
}

std::map<std::string, Hash> static_output_hashes(store_t& store, const derivation_t& drv) {
  return hash_derivation_modulo(store, drv, true).hashes;
}

static derivation_output_t read_derivation_output(source_t& in, const store_dir_config_t& store) {
  const auto path_s = read_string(in);
  const auto hash_algo = read_string(in);
  const auto hash = read_string(in);

  return parse_derivation_output(store, path_s, hash_algo, hash, experimental_feature_settings);
}

string_set_t basic_derivation_t::outputNames() const {
  string_set_t names;
  for (auto& i : outputs)
    names.insert(i.first);
  return names;
}

DerivationOutputsAndOptPaths
basic_derivation_t::outputsAndOptPaths(const store_dir_config_t& store) const {
  DerivationOutputsAndOptPaths outsAndOptPaths;
  for (auto& [output_name, output] : outputs)
    outsAndOptPaths.insert(
        std::make_pair(output_name, std::make_pair(output, output.path(store, name, output_name))));
  return outsAndOptPaths;
}

std::string_view basic_derivation_t::nameFromPath(const store_path_t& drv_path) {
  drv_path.requireDerivation();
  auto nameWithSuffix = drv_path.name();
  nameWithSuffix.remove_suffix(drvExtension.size());
  return nameWithSuffix;
}

source_t& read_derivation(source_t& in, const store_dir_config_t& store, basic_derivation_t& drv,
                       std::string_view name) {
  drv.name = name;

  drv.outputs.clear();
  auto nr = read_num<size_t>(in);
  for (size_t n = 0; n < nr; n++) {
    auto name = read_string(in);
    auto output = read_derivation_output(in, store);
    drv.outputs.emplace(std::move(name), std::move(output));
  }

  drv.input_srcs =
      CommonProto::Serialise<store_path_set_t>::read(store, CommonProto::ReadConn{.from = in});
  in >> drv.platform >> drv.builder;
  drv.args = read_strings<strings_t>(in);

  nr = read_num<size_t>(in);
  for (size_t n = 0; n < nr; n++) {
    auto key = read_string(in);
    auto value = read_string(in);
    drv.env[key] = value;
  }
  drv.structured_attrs = StructuredAttrs::tryExtract(drv.env);

  return in;
}

void write_derivation(sink_t& out, const store_dir_config_t& store, const basic_derivation_t& drv) {
  out << drv.outputs.size();
  for (auto& i : drv.outputs) {
    out << i.first;
    std::visit(overloaded{
                   [&](const derivation_output_t::InputAddressed& doi) {
                     out << store.printStorePath(doi.path) << ""
                         << "";
                   },
                   [&](const derivation_output_t::CAFixed& dof) {
                     out << store.printStorePath(dof.path(store, drv.name, i.first))
                         << dof.ca.printMethodAlgo()
                         << dof.ca.hash.to_string(hash_format_t::base16, false);
                   },
                   [&](const derivation_output_t::CAFloating& dof) {
                     out << ""
                         << (std::string{dof.method.renderPrefix()} + print_hash_algo(dof.hash_algo))
                         << "";
                   },
                   [&](const derivation_output_t::Deferred&) {
                     out << ""
                         << ""
                         << "";
                   },
                   [&](const derivation_output_t::Impure& doi) {
                     out << ""
                         << (std::string{doi.method.renderPrefix()} + print_hash_algo(doi.hash_algo))
                         << "impure";
                   },
               },
               i.second.raw);
  }
  CommonProto::write(store, CommonProto::WriteConn{.to = out}, drv.input_srcs);
  out << drv.platform << drv.builder << drv.args;

  auto write_env = [&](const string_pairs_t aterm_env) {
    out << aterm_env.size();
    for (auto& [k, v] : aterm_env)
      out << k << v;
  };

  StructuredAttrs::checkKeyNotInUse(drv.env);
  if (drv.structured_attrs) {
    string_pairs_t scratch = drv.env;
    scratch.insert(drv.structured_attrs->unparse());
    write_env(scratch);
  } else {
    write_env(drv.env);
  }
}

std::string hash_placeholder(const OutputNameView output_name) {
  // FIXME: memoize?
  return "/" + hash_string(hash_algorithm_t::SHA256, concat_strings("nix-output:", output_name))
                   .to_string(hash_format_t::nix32, false);
}

void basic_derivation_t::applyRewrites(const string_map_t& rewrites) {
  if (rewrites.empty())
    return;

  debug("rewriting the derivation");

  for (auto& rewrite : rewrites)
    debug("rewriting %s as %s", rewrite.first, rewrite.second);

  builder = rewrite_strings(builder, rewrites);
  for (auto& arg : args)
    arg = rewrite_strings(arg, rewrites);

  string_pairs_t new_env;
  for (auto& env_var : env) {
    auto envName = rewrite_strings(env_var.first, rewrites);
    auto envValue = rewrite_strings(env_var.second, rewrites);
    new_env.emplace(envName, envValue);
  }
  env = std::move(new_env);

  if (structured_attrs) {
    // TODO rewrite the JSON AST properly, rather than dump parse round trip.
    auto [_, jsonS] = structured_attrs->unparse();
    jsonS = rewrite_strings(std::move(jsonS), rewrites);
    structured_attrs = StructuredAttrs::parse(jsonS);
  }
}

static void rewrite_derivation(store_t& store, basic_derivation_t& drv, const string_map_t& rewrites) {
  drv.applyRewrites(rewrites);

  auto hash_modulo = hash_derivation_modulo(store, derivation_t(drv), true);
  for (auto& [output_name, output] : drv.outputs) {
    if (std::holds_alternative<derivation_output_t::Deferred>(output.raw)) {
      auto h = get(hash_modulo.hashes, output_name);
      if (!h)
        throw Error("derivation '%s' output '%s' has no hash (derivations.cc/rewriteDerivation)",
                    drv.name, output_name);
      auto out_path = store.makeOutputPath(output_name, *h, drv.name);
      drv.env[output_name] = store.printStorePath(out_path);
      output = derivation_output_t::InputAddressed{
          .path = std::move(out_path),
      };
    }
  }
}

std::optional<basic_derivation_t> derivation_t::try_resolve(store_t& store, store_t* eval_store) const {
  return try_resolve(store,
                    [&](ref<const SingleDerivedPath> drv_path,
                        const std::string& output_name) -> std::optional<store_path_t> {
                      try {
                        return resolve_derived_path(
                            store, SingleDerivedPath::Built{drv_path, output_name}, eval_store);
                      } catch (Error&) {
                        return std::nullopt;
                      }
                    });
}

static bool
try_resolve_input(store_t& store, store_path_set_t& input_srcs, string_map_t& input_rewrites,
                const DownstreamPlaceholder* placeholder_opt, ref<const SingleDerivedPath> drv_path,
                const DerivedPathMap<string_set_t>::ChildNode& input_node,
                std::function<std::optional<store_path_t>(ref<const SingleDerivedPath> drv_path,
                                                       const std::string& output_name)>
                    queryResolutionChain) {
  auto get_placeholder = [&](const std::string& output_name) {
    return placeholder_opt ? DownstreamPlaceholder::unknownDerivation(*placeholder_opt, output_name)
                          : [&] {
                              auto* p = std::get_if<SingleDerivedPath::opaque_t>(&drv_path->raw());
                              // otherwise we should have had a placeholder to build-upon already
                              assert(p);
                              return DownstreamPlaceholder::unknownCaOutput(p->path, output_name);
                            }();
  };

  for (auto& output_name : input_node.value) {
    auto actualPathOpt = queryResolutionChain(drv_path, output_name);
    if (!actualPathOpt)
      return false;
    auto actualPath = *actualPathOpt;
    if (experimental_feature_settings.is_enabled(xp_t::ca_derivations)) {
      input_rewrites.emplace(get_placeholder(output_name).render(), store.printStorePath(actualPath));
    }
    input_srcs.insert(std::move(actualPath));
  }

  for (auto& [output_name, childNode] : input_node.childMap) {
    auto nextPlaceholder = get_placeholder(output_name);
    if (!try_resolve_input(
            store, input_srcs, input_rewrites, &nextPlaceholder,
            make_ref<const SingleDerivedPath>(SingleDerivedPath::Built{drv_path, output_name}),
            childNode, queryResolutionChain))
      return false;
  }
  return true;
}

std::optional<basic_derivation_t>
derivation_t::try_resolve(store_t& store,
                       std::function<std::optional<store_path_t>(ref<const SingleDerivedPath> drv_path,
                                                              const std::string& output_name)>
                           queryResolutionChain) const {
  basic_derivation_t resolved{*this};

  // input_t paths that we'll want to rewrite in the derivation
  string_map_t input_rewrites;

  for (auto& [input_drv, input_node] : input_drvs.map)
    if (!try_resolve_input(store, resolved.input_srcs, input_rewrites, nullptr,
                         make_ref<const SingleDerivedPath>(SingleDerivedPath::opaque_t{input_drv}),
                         input_node, queryResolutionChain))
      return std::nullopt;

  rewrite_derivation(store, resolved, input_rewrites);

  return resolved;
}

/**
 * Process `InputAddressed`, `Deferred`, and `CAFixed` outputs.
 *
 * For `InputAddressed` outputs or `Deferred` outputs:
 *
 * - with `regular` hash kind, validate `InputAddressed` outputs have
 *   the correct path (throws if mismatch). For `Deferred` outputs:
 *   - if `fillIn` is true, fill in the output path to make `InputAddressed`
 *   - if `fillIn` is false, throw an error
 *   Then validate or fill in the environment variable with the path.
 *
 * - with `Deferred` hash kind, validate that the output is either
 *   `InputAddressed` (error) or `Deferred` (correct).
 *
 * For `CAFixed` outputs, validate or fill in the environment variable
 * with the computed path.
 *
 * @tparam fillIn If true, fill in missing output paths and environment
 * variables. If false, validate that all paths are correct (throws on
 * mismatch).
 */
template <bool fillIn>
static void process_derivation_output_paths(store_t& store, auto&& drv, std::string_view drv_name) {
  std::optional<DrvHash> hashesModulo;

  for (auto& [output_name, output] : drv.outputs) {
    auto envHasRightPath = [&](const store_path_t& actual, bool isDeferred = false) {
      if constexpr (fillIn) {
        auto j = drv.env.find(output_name);
        /* Fill in mode: fill in missing or empty environment
           variables */
        if (j == drv.env.end())
          drv.env.insert(j, {output_name, store.printStorePath(actual)});
        else if (j->second == "")
          j->second = store.printStorePath(actual);
        /* We know validation will succeed after fill-in, but
           just to be extra sure, validate unconditionally */
      }
      auto j = drv.env.find(output_name);
      if (j == drv.env.end())
        throw Error(
            "derivation has missing environment variable '%s', should be '%s' but is not present",
            output_name, store.printStorePath(actual));
      if (j->second != store.printStorePath(actual)) {
        if (isDeferred)
          warn("derivation has incorrect environment variable '%s', should be '%s' but is actually "
               "'%s'\nThis will be an error in future versions of Nix; compatibility of CA "
               "derivations will be broken.",
               output_name, store.printStorePath(actual), j->second);
        else
          throw Error("derivation has incorrect environment variable '%s', should be '%s' but is "
                      "actually '%s'",
                      output_name, store.printStorePath(actual), j->second);
      }
    };
    auto hash = [&]<typename Output>(const Output& outputVariant) {
      if (!hashesModulo) {
        // somewhat expensive so we do lazily
        hashesModulo = hash_derivation_modulo(store, drv, true);
      }
      switch (hashesModulo->kind) {
        case DrvHash::Kind::regular: {
          auto h = get(hashesModulo->hashes, output_name);
          if (!h)
            throw Error("derivation produced no hash for output '%s'", output_name);
          auto out_path = store.makeOutputPath(output_name, *h, drv_name);

          if constexpr (std::is_same_v<Output, derivation_output_t::InputAddressed>) {
            if (outputVariant.path == out_path) {
              return; // Correct case
            }
            /* Error case, an explicitly wrong path is
               always an error. */
            throw Error("derivation has incorrect output '%s', should be '%s'",
                        store.printStorePath(outputVariant.path), store.printStorePath(out_path));
          } else if constexpr (std::is_same_v<Output, derivation_output_t::Deferred>) {
            if constexpr (fillIn)
              /* Fill in output path for Deferred
                 outputs */
              output = derivation_output_t::InputAddressed{
                  .path = out_path,
              };
            else
              /* Validation mode: deferred outputs
                 should have been filled in */
              warn("derivation has incorrect deferred output, should be '%s'.\nThis will be an "
                   "error in future versions of Nix; compatibility of CA derivations will be "
                   "broken.",
                   store.printStorePath(out_path));
          } else {
            /* Will never happen, based on where
               `hash` is called. */
            static_assert(false);
          }
          envHasRightPath(out_path);
          break;
        }
        case DrvHash::Kind::Deferred:
          if constexpr (std::is_same_v<Output, derivation_output_t::InputAddressed>) {
            /* Error case, an explicitly wrong path is
               always an error. */
            throw Error("derivation has incorrect output '%s', should be deferred",
                        store.printStorePath(outputVariant.path));
          } else if constexpr (std::is_same_v<Output, derivation_output_t::Deferred>) {
            /* Correct: Deferred output with Deferred
               hash kind. */
          } else {
            /* Will never happen, based on where
               `hash` is called. */
            static_assert(false);
          }
          break;
      }
    };
    std::visit(overloaded{
                   [&](const derivation_output_t::InputAddressed& o) { hash(o); },
                   [&](const derivation_output_t::Deferred& o) { hash(o); },
                   [&](const derivation_output_t::CAFixed& dof) {
                     envHasRightPath(dof.path(store, drv_name, output_name));
                   },
                   [&](const auto&) {
                     // Nothing to do for other output types
                   },
               },
               output.raw);
  }

  /* Don't need the answer, but do this anyways to assert is proper
     combination. The code above is more general and naturally allows
     combinations that are currently prohibited. */
  drv.type();
}

void derivation_t::checkInvariants(store_t& store, const store_path_t& drv_path) const {
  assert(drv_path.is_derivation());
  std::string drv_name(drv_path.name());
  drv_name = drv_name.substr(0, drv_name.size() - drvExtension.size());

  if (drv_name != name) {
    throw Error("derivation '%s' has name '%s' which does not match its path",
                store.printStorePath(drv_path), name);
  }

  try {
    checkInvariants(store);
  } catch (Error& e) {
    e.add_trace({}, "while checking derivation '%s'", store.printStorePath(drv_path));
    throw;
  }
}

void derivation_t::checkInvariants(store_t& store) const {
  process_derivation_output_paths<false>(store, *this, name);
}

void derivation_t::fillInOutputPaths(store_t& store) {
  process_derivation_output_paths<true>(store, *this, name);
}

derivation_t derivation_t::parseJsonAndValidate(store_t& store, const nlohmann::json& json) {
  auto drv = static_cast<derivation_t>(json);

  drv.fillInOutputPaths(store);

  try {
    drv.checkInvariants(store);
  } catch (Error& e) {
    e.add_trace({}, "while checking derivation from JSON with name '%s'", drv.name);
    throw;
  }

  return drv;
}

const Hash impure_output_hash = hash_string(hash_algorithm_t::SHA256, "impure");

} // namespace nix

namespace nlohmann {

using namespace nix;

void adl_serializer<derivation_output_t>::to_json(json& res, const derivation_output_t& o) {
  res = nlohmann::json::object();
  std::visit(overloaded{
                 [&](const derivation_output_t::InputAddressed& doi) { res["path"] = doi.path; },
                 [&](const derivation_output_t::CAFixed& dof) {
                   res = dof.ca;
    // FIXME print refs?
    /* it would be nice to output the path for user convenience, but
       this would require us to know the store dir. */
#if 0
                res["path"] = dof.path(store, drv_name, output_name);
#endif
                 },
                 [&](const derivation_output_t::CAFloating& dof) {
                   res["method"] = std::string{dof.method.render()};
                   res["hashAlgo"] = print_hash_algo(dof.hash_algo);
                 },
                 [&](const derivation_output_t::Deferred&) {},
                 [&](const derivation_output_t::Impure& doi) {
                   res["method"] = std::string{doi.method.render()};
                   res["hashAlgo"] = print_hash_algo(doi.hash_algo);
                   res["impure"] = true;
                 },
             },
             o.raw);
}

derivation_output_t
adl_serializer<derivation_output_t>::from_json(const json& _json,
                                            const experimental_feature_settings_t& xp_settings) {
  std::set<std::string_view> keys;
  auto& json = get_object(_json);

  for (const auto& [key, _] : json)
    keys.insert(key);

  auto methodAlgo = [&]() -> std::pair<content_address_method_t, hash_algorithm_t> {
    content_address_method_t method = content_address_method_t::parse(get_string(value_at(json, "method")));
    if (method == content_address_method_t::raw_t::Text)
      xp_settings.require(xp_t::dynamic_derivations, "text-hashed derivation output in JSON");

    auto hash_algo = parse_hash_algo(get_string(value_at(json, "hashAlgo")));
    return {std::move(method), std::move(hash_algo)};
  };

  if (keys == (std::set<std::string_view>{"path"})) {
    return derivation_output_t::InputAddressed{
        .path = value_at(json, "path"),
    };
  }

  else if (keys == (std::set<std::string_view>{"method", "hash"})) {
    auto dof = derivation_output_t::CAFixed{
        .ca = static_cast<content_address_t>(_json),
    };
    if (dof.ca.method == content_address_method_t::raw_t::Text)
      xp_settings.require(xp_t::dynamic_derivations, "text-hashed derivation output in JSON");
    /* We no longer produce this (denormalized) field (for the
       reasons described above), so we don't need to check it. */
#if 0
        if (dof.path(store, drv_name, output_name) != static_cast<store_path_t>(value_at(json, "path")))
            throw Error("Path doesn't match derivation output");
#endif
    return dof;
  }

  else if (keys == (std::set<std::string_view>{"method", "hashAlgo"})) {
    xp_settings.require(xp_t::ca_derivations);
    auto [method, hash_algo] = methodAlgo();
    return derivation_output_t::CAFloating{
        .method = std::move(method),
        .hash_algo = std::move(hash_algo),
    };
  }

  else if (keys == (std::set<std::string_view>{})) {
    return derivation_output_t::Deferred{};
  }

  else if (keys == (std::set<std::string_view>{"method", "hashAlgo", "impure"})) {
    xp_settings.require(xp_t::impure_derivations);
    auto [method, hash_algo] = methodAlgo();
    return derivation_output_t::Impure{
        .method = std::move(method),
        .hash_algo = hash_algo,
    };
  }

  else {
    throw Error("invalid JSON for derivation output");
  }
}

void adl_serializer<derivation_t>::to_json(json& res, const derivation_t& d) {
  res = nlohmann::json::object();

  res["name"] = d.name;

  res["version"] = expectedJsonVersionDerivation;

  {
    nlohmann::json& outputsObj = res["outputs"];
    outputsObj = nlohmann::json::object();
    for (auto& [output_name, output] : d.outputs) {
      outputsObj[output_name] = output;
    }
  }

  {
    auto& inputsObj = res["inputs"];
    inputsObj = nlohmann::json::object();

    {
      auto& inputsList = inputsObj["srcs"];
      inputsList = nlohmann::json::array();
      for (auto& input : d.input_srcs)
        inputsList.emplace_back(input);
    }

    auto doInput = [&](this const auto& doInput, const auto& input_node) -> nlohmann::json {
      auto value = nlohmann::json::object();
      value["outputs"] = input_node.value;
      {
        auto next = nlohmann::json::object();
        for (auto& [output_id, childNode] : input_node.childMap)
          next[output_id] = doInput(childNode);
        value["dynamicOutputs"] = std::move(next);
      }
      return value;
    };

    auto& inputDrvsObj = inputsObj["drvs"];
    inputDrvsObj = nlohmann::json::object();
    for (auto& [input_drv, input_node] : d.input_drvs.map) {
      inputDrvsObj[input_drv.to_string()] = doInput(input_node);
    }
  }

  res["system"] = d.platform;
  res["builder"] = d.builder;
  res["args"] = d.args;
  res["env"] = d.env;

  if (d.structured_attrs)
    res["structuredAttrs"] = d.structured_attrs->structured_attrs;
}

derivation_t adl_serializer<derivation_t>::from_json(const json& _json,
                                                 const experimental_feature_settings_t& xp_settings) {
  using nlohmann::detail::value_t;

  derivation_t res;

  auto& json = get_object(_json);

  res.name = get_string(value_at(json, "name"));

  {
    auto version = get_unsigned(value_at(json, "version"));
    if (value_at(json, "version") != expectedJsonVersionDerivation)
      throw Error("Unsupported derivation JSON format version %d, only format version %d is "
                  "currently supported.",
                  version, expectedJsonVersionDerivation);
  }

  try {
    auto outputs = get_object(value_at(json, "outputs"));
    for (auto& [output_name, output] : outputs) {
      res.outputs.insert_or_assign(output_name,
                                   adl_serializer<derivation_output_t>::from_json(output, xp_settings));
    }
  } catch (Error& e) {
    e.add_trace({}, "while reading key 'outputs'");
    throw;
  }

  try {
    auto inputsObj = get_object(value_at(json, "inputs"));

    try {
      auto input_srcs = get_array(value_at(inputsObj, "srcs"));
      for (auto& input : input_srcs)
        res.input_srcs.insert(input);
    } catch (Error& e) {
      e.add_trace({}, "while reading key 'srcs'");
      throw;
    }

    try {
      auto doInput = [&](this const auto& doInput,
                         const auto& _json) -> DerivedPathMap<string_set_t>::ChildNode {
        auto& json = get_object(_json);
        DerivedPathMap<string_set_t>::ChildNode node;
        node.value = get_string_set(value_at(json, "outputs"));
        auto drvs = get_object(value_at(json, "dynamicOutputs"));
        for (auto& [output_id, childNode] : drvs) {
          xp_settings.require(xp_t::dynamic_derivations,
                             [&] { return fmt("dynamic output '%s' in JSON", output_id); });
          node.childMap[output_id] = doInput(childNode);
        }
        return node;
      };
      auto drvs = get_object(value_at(inputsObj, "drvs"));
      for (auto& [inputDrvPath, inputOutputs] : drvs)
        res.input_drvs.map[store_path_t{inputDrvPath}] = doInput(inputOutputs);
    } catch (Error& e) {
      e.add_trace({}, "while reading key 'drvs'");
      throw;
    }
  } catch (Error& e) {
    e.add_trace({}, "while reading key 'inputs'");
    throw;
  }

  res.platform = get_string(value_at(json, "system"));
  res.builder = get_string(value_at(json, "builder"));
  res.args = get_string_list(value_at(json, "args"));

  auto envJson = value_at(json, "env");
  try {
    res.env = get_string_map(envJson);
  } catch (Error& e) {
    e.add_trace({}, "while reading key 'env'");
    throw;
  }

  if (auto structured_attrs = get(json, "structuredAttrs"))
    res.structured_attrs = StructuredAttrs{*structured_attrs};

  return res;
}

} // namespace nlohmann
