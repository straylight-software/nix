#include "nix/fetchers/cache.h"
#include "nix/fetchers/fetch-settings.h"
#include "nix/fetchers/fetch-to-store.h"
#include "nix/fetchers/fetchers.h"
#include "nix/store/store-api.h"
#include "nix/util/archive.h"

namespace nix::fetchers {

struct path_input_scheme_t : InputScheme {
  std::optional<Input> inputFromURL(const settings_t& settings, const parsed_url_t& url,
                                    bool require_tree) const override {
    if (url.scheme != "path")
      return {};

    if (url.authority && url.authority->host.size())
      throw Error("path URL '%s' should not have an authority ('%s')", url, *url.authority);

    Input input{};
    input.attrs.insert_or_assign("type", "path");
    input.attrs.insert_or_assign("path", render_url_path_ensure_legal(url.path));

    for (auto& [name, value] : url.query)
      if (name == "rev" || name == "narHash")
        input.attrs.insert_or_assign(name, value);
      else if (name == "revCount" || name == "lastModified") {
        if (auto n = string2_int<uint64_t>(value))
          input.attrs.insert_or_assign(name, *n);
        else
          throw Error("path URL '%s' has invalid parameter '%s'", url, name);
      } else
        throw Error("path URL '%s' has unsupported parameter '%s'", url, name);

    return input;
  }

  std::string_view schemeName() const override { return "path"; }

  std::string schemeDescription() const override {
    // TODO
    return "";
  }

  const std::map<std::string, AttributeInfo>& allowed_attrs() const override {
    static const std::map<std::string, AttributeInfo> attrs = {
        {
            "path",
            {},
        },
        /* Allow the user to pass in "fake" tree info
           attributes. This is useful for making a pinned tree work
           the same as the repository from which is exported (e.g.
           path:/nix/store/...-source?last_modified=1585388205&rev=b0c285...).
         */
        {
            "rev",
            {},
        },
        {
            "revCount",
            {},
        },
        {
            "lastModified",
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
    get_str_attr(attrs, "path");

    Input input{};
    input.attrs = attrs;
    return input;
  }

  parsed_url_t toURL(const Input& input, bool abbreviate) const override {
    auto query = attrs_to_query(input.attrs);
    query.erase("path");
    query.erase("type");
    query.erase("__final");
    return parsed_url_t{
        .scheme = "path",
        .path = split_string<std::vector<std::string>>(get_str_attr(input.attrs, "path"), "/"),
        .query = query,
    };
  }

  std::optional<std::filesystem::path> get_source_path(const Input& input) const override {
    return get_abs_path(input);
  }

  void putFile(const Input& input, const canon_path_t& path, std::string_view contents,
               std::optional<std::string> commit_msg) const override {
    write_file(get_abs_path(input) / path.rel(), contents);
  }

  std::optional<std::string> isRelative(const Input& input) const override {
    auto path = get_str_attr(input.attrs, "path");
    if (is_absolute(path))
      return std::nullopt;
    else
      return path;
  }

  bool isLocked(const settings_t& settings, const Input& input) const override {
    return (bool)input.getNarHash();
  }

  std::filesystem::path get_abs_path(const Input& input) const {
    auto path = get_str_attr(input.attrs, "path");

    if (is_absolute(path))
      return canon_path(path);

    throw Error("cannot fetch input '%s' because it uses a relative path", input.to_string());
  }

  std::pair<ref<SourceAccessor>, Input> get_accessor(const settings_t& settings, Store& store,
                                                    const Input& _input) const override {
    Input input(_input);

    auto abs_path = get_abs_path(input);

    // FIXME: check whether access to 'path' is allowed.

    auto accessor = make_fs_source_accessor(abs_path);

    auto store_path = store.maybeParseStorePath(abs_path.string());

    if (store_path) {
      store.addTempRoot(*store_path);

      // To prevent `fetchToStore()` copying the path again to Nix
      // store, pre-create an entry in the fetcher cache.
      auto info = store.maybeQueryPathInfo(*store_path);
      if (info) {
        accessor->fingerprint = fmt("path:%s", info->nar_hash.to_string(hash_format_t::SRI, true));
        settings.get_cache()->upsert(
            make_source_path_to_hash_cache_key(*accessor->fingerprint,
                                         ContentAddressMethod::raw_t::nix_archive, "/"),
            {{"hash", info->nar_hash.to_string(hash_format_t::SRI, true)}});
      }
    }

    return {accessor, std::move(input)};
  }
};

static auto r_path_input_scheme =
    on_startup_t([] { register_input_scheme(std::make_unique<path_input_scheme_t>()); });

} // namespace nix::fetchers
