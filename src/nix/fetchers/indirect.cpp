#include "nix/fetchers/fetchers.h"
#include "nix/fetchers/git-utils.h"
#include "nix/store/path.h"
#include "nix/util/url-parts.h"

namespace nix::fetchers {

std::regex flake_regex("[a-zA-Z][a-zA-Z0-9_-]*", std::regex::ECMAScript);

struct indirect_input_scheme_t : InputScheme {
  std::optional<Input> inputFromURL(const settings_t& settings, const parsed_url_t& url,
                                    bool require_tree) const override {
    if (url.scheme != "flake")
      return {};

    /* This ignores empty path segments for back-compat. Older versions used a tokenize_string here.
     */
    auto path = url.path_segments(/*skip_empty=*/true) | std::ranges::to<std::vector<std::string>>();

    std::optional<Hash> rev;
    std::optional<std::string> ref;

    if (path.size() == 1) {
    } else if (path.size() == 2) {
      if (std::regex_match(path[1], rev_regex))
        rev = Hash::parse_any(path[1], hash_algorithm_t::SHA1);
      else if (is_legal_ref_name(path[1]))
        ref = path[1];
      else
        throw BadURL("in flake URL '%s', '%s' is not a commit hash or branch/tag name", url,
                     path[1]);
    } else if (path.size() == 3) {
      if (!is_legal_ref_name(path[1]))
        throw BadURL("in flake URL '%s', '%s' is not a branch/tag name", url, path[1]);
      ref = path[1];
      if (!std::regex_match(path[2], rev_regex))
        throw BadURL("in flake URL '%s', '%s' is not a commit hash", url, path[2]);
      rev = Hash::parse_any(path[2], hash_algorithm_t::SHA1);
    } else
      throw BadURL("GitHub URL '%s' is invalid", url);

    std::string id = path[0];
    if (!std::regex_match(id, flake_regex))
      throw BadURL("'%s' is not a valid flake ID", id);

    // FIXME: forbid query params?

    Input input{};
    input.attrs.insert_or_assign("type", "indirect");
    input.attrs.insert_or_assign("id", id);
    if (rev)
      input.attrs.insert_or_assign("rev", rev->git_rev());
    if (ref)
      input.attrs.insert_or_assign("ref", *ref);

    return input;
  }

  std::string_view schemeName() const override { return "indirect"; }

  std::string schemeDescription() const override {
    // TODO
    return "";
  }

  const std::map<std::string, AttributeInfo>& allowed_attrs() const override {
    static const std::map<std::string, AttributeInfo> attrs = {
        {
            "id",
            {},
        },
        {
            "ref",
            {},
        },
        {
            "rev",
            {},
        },
        {
            "narHash",
            {},
        },
    };
    return attrs;
  }

  std::optional<Input> inputFromAttrs(const settings_t& settings, const Attrs& attrs) const override {
    auto id = get_str_attr(attrs, "id");
    if (!std::regex_match(id, flake_regex))
      throw BadURL("'%s' is not a valid flake ID", id);

    Input input{};
    input.attrs = attrs;
    return input;
  }

  parsed_url_t toURL(const Input& input, bool abbreviate) const override {
    parsed_url_t url{
        .scheme = "flake",
        .path = {get_str_attr(input.attrs, "id")},
    };
    if (auto ref = input.getRef()) {
      url.path.push_back(*ref);
    };
    if (auto rev = input.getRev()) {
      url.path.push_back(rev->git_rev());
    };
    return url;
  }

  Input applyOverrides(const Input& _input, std::optional<std::string> ref,
                       std::optional<Hash> rev) const override {
    auto input(_input);
    if (rev)
      input.attrs.insert_or_assign("rev", rev->git_rev());
    if (ref)
      input.attrs.insert_or_assign("ref", *ref);
    return input;
  }

  std::pair<ref<SourceAccessor>, Input> get_accessor(const settings_t& settings, Store& store,
                                                    const Input& input) const override {
    throw Error("indirect input '%s' cannot be fetched directly", input.to_string());
  }

  bool isDirect(const Input& input) const override { return false; }
};

static auto r_indirect_input_scheme =
    on_startup_t([] { register_input_scheme(std::make_unique<indirect_input_scheme_t>()); });

} // namespace nix::fetchers
