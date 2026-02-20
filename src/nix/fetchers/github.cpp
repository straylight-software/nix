#include <fstream>
#include <optional>

#include <nlohmann/json.hpp>

#include "nix/fetchers/cache.h"
#include "nix/fetchers/fetch-settings.h"
#include "nix/fetchers/fetchers.h"
#include "nix/fetchers/git-utils.h"
#include "nix/fetchers/tarball.h"
#include "nix/store/filetransfer.h"
#include "nix/store/globals.h"
#include "nix/store/store-api.h"
#include "nix/util/git.h"
#include "nix/util/tarfile.h"
#include "nix/util/types.h"
#include "nix/util/url-parts.h"

namespace nix::fetchers {

struct download_url_t {
  parsed_url_t url;
  headers_t headers;
};

// A github, gitlab, or sourcehut host
const static std::string host_regex_s = "[a-zA-Z0-9.-]*"; // FIXME: check
std::regex host_regex(host_regex_s, std::regex::ECMAScript);

struct git_archive_input_scheme_t : input_scheme_t {
  virtual std::optional<std::pair<std::string, std::string>>
  accessHeaderFromToken(const std::string& token) const = 0;

  std::optional<input_t> inputFromURL(const fetchers::settings_t& settings, const parsed_url_t& url,
                                      bool require_tree) const override {
    if (url.scheme() != schemeName())
      return {};

    /* This ignores empty path segments for back-compat. Older versions used a tokenize_string here.
     */
    auto path =
        url.path_segments(/*skip_empty=*/true) | std::ranges::to<std::vector<std::string>>();

    std::optional<std::string> rev;
    std::optional<std::string> ref;
    std::optional<std::string> host_url;

    auto size = path.size();
    if (size == 3) {
      if (std::regex_match(path[2], rev_regex))
        rev = path[2];
      else
        ref = path[2];
    } else if (size > 3) {
      std::string rs;
      for (auto i = std::next(path.begin(), 2); i != path.end(); i++) {
        rs += *i;
        if (std::next(i) != path.end()) {
          rs += "/";
        }
      }
      ref = rs;
    } else if (size < 2)
      throw BadURL("URL '%s' is invalid", url);

    for (auto& [name, value] : url.query()) {
      if (name == "rev") {
        if (rev)
          throw BadURL("URL '%s' contains multiple commit hashes", url);
        rev = value;
      } else if (name == "ref") {
        if (ref)
          throw BadURL("URL '%s' contains multiple branch/tag names", url);
        ref = value;
      } else if (name == "host")
        host_url = value;
      // FIXME: barf on unsupported attributes
    }

    Attrs attrs;
    attrs.insert_or_assign("type", std::string{schemeName()});
    attrs.insert_or_assign("owner", path[0]);
    attrs.insert_or_assign("repo", path[1]);
    if (rev)
      attrs.insert_or_assign("rev", *rev);
    if (ref)
      attrs.insert_or_assign("ref", *ref);
    if (host_url)
      attrs.insert_or_assign("host", *host_url);

    auto nar_hash = url.query().find("narHash");
    if (nar_hash != url.query().end())
      attrs.insert_or_assign("narHash", nar_hash->second);

    return inputFromAttrs(settings, attrs);
  }

  const std::map<std::string, AttributeInfo>& allowed_attrs() const override {
    static const std::map<std::string, AttributeInfo> attrs = {
        {
            "owner",
            {},
        },
        {
            "repo",
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
        {
            "lastModified",
            {},
        },
        {
            "host",
            {},
        },
        {
            "treeHash",
            {},
        },
    };
    return attrs;
  }

  std::optional<input_t> inputFromAttrs(const fetchers::settings_t& settings,
                                        const Attrs& attrs) const override {
    get_str_attr(attrs, "owner");
    get_str_attr(attrs, "repo");

    auto ref = maybe_get_str_attr(attrs, "ref");
    auto rev = maybe_get_str_attr(attrs, "rev");
    if (ref && rev)
      throw BadURL("input %s contains both a commit hash ('%s') and a branch/tag name ('%s')",
                   attrs_to_json(attrs), *rev, *ref);

    if (rev)
      Hash::parse_any(*rev, hash_algorithm_t::SHA1);

    if (ref && !is_legal_ref_name(*ref))
      throw BadURL("input %s contains an invalid branch/tag name", attrs_to_json(attrs));

    if (auto host = maybe_get_str_attr(attrs, "host"); host && !std::regex_match(*host, host_regex))
      throw BadURL("input %s contains an invalid instance host", attrs_to_json(attrs));

    input_t input{};
    input.attrs = attrs;
    return input;
  }

  parsed_url_t toURL(const input_t& input, bool abbreviate) const override {
    auto owner = get_str_attr(input.attrs, "owner");
    auto repo = get_str_attr(input.attrs, "repo");
    auto ref = input.getRef();
    auto rev = input.getRev();
    std::vector<std::string> path{owner, repo};
    assert(!(ref && rev));
    if (ref)
      path.push_back(*ref);
    if (rev)
      path.push_back(abbreviate ? rev->git_short_rev() : rev->git_rev());
    parsed_url_t url;
    url.set_scheme(std::string{schemeName()});
    url.set_path(path);
    if (auto nar_hash = input.getNarHash())
      url.query().insert_or_assign("narHash", nar_hash->to_string(hash_format_t::sri, true));
    auto host = maybe_get_str_attr(input.attrs, "host");
    if (host)
      url.query().insert_or_assign("host", *host);
    return url;
  }

  input_t applyOverrides(const input_t& _input, std::optional<std::string> ref,
                         std::optional<Hash> rev) const override {
    auto input(_input);
    if (rev && ref)
      throw BadURL(
          "cannot apply both a commit hash (%s) and a branch/tag name ('%s') to input '%s'",
          rev->git_rev(), *ref, input.to_string());
    if (rev) {
      input.attrs.insert_or_assign("rev", rev->git_rev());
      input.attrs.erase("ref");
    }
    if (ref) {
      input.attrs.insert_or_assign("ref", *ref);
      input.attrs.erase("rev");
    }
    return input;
  }

  // Search for the longest possible match starting from the beginning and ending at either the end
  // or a path segment.
  std::optional<std::string> getAccessToken(const fetchers::settings_t& settings,
                                            const std::string& host,
                                            const std::string& url) const override {
    auto tokens = settings.accessTokens.get();
    std::string answer;
    size_t answer_match_len = 0;
    if (!url.empty()) {
      for (auto& token : tokens) {
        auto first = url.find(token.first);
        if (first != std::string::npos && token.first.length() > answer_match_len && first == 0 &&
            url.substr(0, token.first.length()) == token.first &&
            (url.length() == token.first.length() || url[token.first.length()] == '/')) {
          answer = token.second;
          answer_match_len = token.first.length();
        }
      }
      if (!answer.empty())
        return answer;
    }
    if (auto token = get(tokens, host))
      return *token;
    return {};
  }

  headers_t make_headers_with_auth_tokens(const fetchers::settings_t& settings,
                                          const std::string& host, const input_t& input) const {
    auto owner = get_str_attr(input.attrs, "owner");
    auto repo = get_str_attr(input.attrs, "repo");
    auto host_and_path = fmt("%s/%s/%s", host, owner, repo);
    return make_headers_with_auth_tokens(settings, host, host_and_path);
  }

  headers_t make_headers_with_auth_tokens(const fetchers::settings_t& settings,
                                          const std::string& host,
                                          const std::string& host_and_path) const {
    headers_t headers;
    auto access_token = getAccessToken(settings, host, host_and_path);
    if (access_token) {
      auto hdr = accessHeaderFromToken(*access_token);
      if (hdr)
        headers.push_back(*hdr);
      else
        warn("Unrecognized access token for host '%s'", host);
    }
    return headers;
  }

  struct ref_info_t {
    Hash rev;
    std::optional<Hash> tree_hash;
  };

  virtual ref_info_t get_rev_from_ref(const settings_t& settings, nix::store_t& store,
                                      const input_t& input) const = 0;

  virtual download_url_t get_download_url(const settings_t& settings,
                                          const input_t& input) const = 0;

  struct tarball_info_t {
    Hash tree_hash;
    time_t last_modified;
  };

  std::pair<input_t, tarball_info_t> download_archive(const settings_t& settings, store_t& store,
                                                      input_t input) const {
    if (!maybe_get_str_attr(input.attrs, "ref"))
      input.attrs.insert_or_assign("ref", "HEAD");

    std::optional<Hash> upstreamTreeHash;

    auto rev = input.getRev();
    if (!rev) {
      auto ref_info = get_rev_from_ref(settings, store, input);
      rev = ref_info.rev;
      upstreamTreeHash = ref_info.tree_hash;
      debug("HEAD revision for '%s' is %s", input.to_string(), ref_info.rev.git_rev());
    }

    input.attrs.erase("ref");
    input.attrs.insert_or_assign("rev", rev->git_rev());

    auto cache = settings.get_cache();

    cache_t::Key tree_hash_key{"gitRevToTreeHash", {{"rev", rev->git_rev()}}};
    cache_t::Key last_modified_key{"gitRevToLastModified", {{"rev", rev->git_rev()}}};

    if (auto treeHashAttrs = cache->lookup(tree_hash_key)) {
      if (auto lastModifiedAttrs = cache->lookup(last_modified_key)) {
        auto tree_hash = get_rev_attr(*treeHashAttrs, "treeHash");
        auto last_modified = get_int_attr(*lastModifiedAttrs, "lastModified");
        if (settings.getTarballCache()->hasObject(tree_hash))
          return {std::move(input),
                  tarball_info_t{.tree_hash = tree_hash, .last_modified = (time_t)last_modified}};
        else
          debug("Git tree with hash '%s' has disappeared from the cache, refetching...",
                tree_hash.git_rev());
      }
    }

    /* Stream the tarball into the tarball cache. */
    auto url = get_download_url(settings, input);

    auto source = sink_to_source([&](sink_t& sink) {
      FileTransferRequest req(url.url);
      req.headers = url.headers;
      get_file_transfer()->download(std::move(req), sink);
    });

    auto act =
        std::make_unique<activity_t>(*logger, lvl_info, act_unknown,
                                     fmt("unpacking '%s' into the Git cache", input.to_string()));

    tar_archive_t archive{*source};
    auto tarball_cache = settings.getTarballCache();
    auto parse_sink = tarball_cache->get_file_system_object_sink();
    auto last_modified = unpack_tarfile_to_sink(archive, *parse_sink);
    auto tree = parse_sink->flush();

    act.reset();

    tarball_info_t tarball_info{.tree_hash = tarball_cache->dereferenceSingletonDirectory(tree),
                                .last_modified = last_modified};

    cache->upsert(tree_hash_key, Attrs{{"treeHash", tarball_info.tree_hash.git_rev()}});
    cache->upsert(last_modified_key, Attrs{{"lastModified", (uint64_t)tarball_info.last_modified}});

#if 0
        if (upstreamTreeHash != tarball_info.tree_hash)
            warn(
                "Git tree hash mismatch for revision '%s' of '%s': "
                "expected '%s', got '%s'. "
                "This can happen if the Git repository uses submodules.",
                rev->git_rev(), input.to_string(), upstreamTreeHash->git_rev(), tarball_info.tree_hash.git_rev());
#endif

    return {std::move(input), tarball_info};
  }

  std::pair<ref<source_accessor_t>, input_t>
  get_accessor(const settings_t& settings, store_t& store, const input_t& _input) const override {
    auto [input, tarball_info] = download_archive(settings, store, _input);

#if 0
        input.attrs.insert_or_assign("treeHash", tarball_info.tree_hash.git_rev());
#endif
    input.attrs.insert_or_assign("lastModified", uint64_t(tarball_info.last_modified));

    auto accessor = settings.getTarballCache()->get_accessor(tarball_info.tree_hash, {},
                                                             "«" + input.to_string(true) + "»");

    if (!settings.trustTarballsFromGitForges)
      // FIXME: computing the NAR hash here is wasteful if
      // copyInputToStore() is just going to hash/copy it as
      // well.
      input.attrs.insert_or_assign(
          "narHash", accessor->hash_path(canon_path_t::root).to_string(hash_format_t::sri, true));

    return {accessor, input};
  }

  bool isLocked(const settings_t& settings, const input_t& input) const override {
    /* Since we can't verify the integrity of the tarball from the
       git revision alone, we also require a NAR hash for
       locking. FIXME: in the future, we may want to require a git
       tree hash instead of a NAR hash. */
    return input.getRev().has_value() &&
           (settings.trustTarballsFromGitForges || input.getNarHash().has_value());
  }

  std::optional<std::string> get_fingerprint(store_t& store, const input_t& input) const override {
    if (auto rev = input.getRev())
      return "github:" + rev->git_rev();
    else
      return std::nullopt;
  }
};

struct git_hub_input_scheme_t : git_archive_input_scheme_t {
  std::string_view schemeName() const override { return "github"; }

  std::string schemeDescription() const override {
    // TODO
    return "";
  }

  std::optional<std::pair<std::string, std::string>>
  accessHeaderFromToken(const std::string& token) const override {
    // Github supports PAT/OAuth2 tokens and HTTP Basic
    // Authentication.  The former simply specifies the token, the
    // latter can use the token as the password.  Only the first
    // is used here. See
    // https://developer.github.com/v3/#authentication and
    // https://docs.github.com/en/developers/apps/authorizing-oath-apps
    return std::pair<std::string, std::string>("Authorization", fmt("token %s", token));
  }

  std::string getHost(const input_t& input) const {
    return maybe_get_str_attr(input.attrs, "host").value_or("github.com");
  }

  std::string getOwner(const input_t& input) const { return get_str_attr(input.attrs, "owner"); }

  std::string getRepo(const input_t& input) const { return get_str_attr(input.attrs, "repo"); }

  ref_info_t get_rev_from_ref(const settings_t& settings, nix::store_t& store,
                              const input_t& input) const override {
    auto host = getHost(input);
    auto url = fmt(host == "github.com" ? "https://api.%s/repos/%s/%s/commits/%s"
                                        : "https://%s/api/v3/repos/%s/%s/commits/%s",
                   host, getOwner(input), getRepo(input), *input.getRef());

    headers_t headers = make_headers_with_auth_tokens(settings, host, input);

    auto downloadResult = download_file(store, settings, url, "source", headers);
    auto json = nlohmann::json::parse(
        store.requireStoreObjectAccessor(downloadResult.store_path)->read_file(canon_path_t::root));

    return ref_info_t{.rev = Hash::parse_any(std::string{json["sha"]}, hash_algorithm_t::SHA1),
                      .tree_hash = Hash::parse_any(std::string{json["commit"]["tree"]["sha"]},
                                                   hash_algorithm_t::SHA1)};
  }

  download_url_t get_download_url(const settings_t& settings, const input_t& input) const override {
    auto host = getHost(input);

    headers_t headers = make_headers_with_auth_tokens(settings, host, input);

    // If we have no auth headers then we default to the public archive
    // urls so we do not run into rate limits.
    const auto urlFmt = host != "github.com" ? "https://%s/api/v3/repos/%s/%s/tarball/%s"
                        : headers.empty()    ? "https://%s/%s/%s/archive/%s.tar.gz"
                                             : "https://api.%s/repos/%s/%s/tarball/%s";

    const auto url = fmt(urlFmt, host, getOwner(input), getRepo(input), input.getRev()->git_rev());

    return download_url_t{parse_url(url), headers};
  }

  void clone(const settings_t& settings, store_t& store, const input_t& input,
             const std::filesystem::path& dest_dir) const override {
    auto host = getHost(input);
    input_t::fromURL(settings,
                     fmt("git+https://%s/%s/%s.git", host, getOwner(input), getRepo(input)))
        .applyOverrides(input.getRef(), input.getRev())
        .clone(settings, store, dest_dir);
  }
};

struct git_lab_input_scheme_t : git_archive_input_scheme_t {
  std::string_view schemeName() const override { return "gitlab"; }

  std::string schemeDescription() const override {
    // TODO
    return "";
  }

  std::optional<std::pair<std::string, std::string>>
  accessHeaderFromToken(const std::string& token) const override {
    // Gitlab supports 4 kinds of authorization, two of which are
    // relevant here: OAuth2 and PAT (Private Access Token).  The
    // user can indicate which token is used by specifying the
    // token as <TYPE>:<VALUE>, where type is "OAuth2" or "PAT".
    // If the <TYPE> is unrecognized, this will fall back to
    // treating this simply has <HDRNAME>:<HDRVAL>.  See
    // https://docs.gitlab.com/12.10/ee/api/README.html#authentication
    auto fldsplit = token.find_first_of(':');
    // n.b. C++20 would allow: if (token.starts_with("OAuth2:")) ...
    if ("OAuth2" == token.substr(0, fldsplit))
      return std::make_pair("Authorization", fmt("Bearer %s", token.substr(fldsplit + 1)));
    if ("PAT" == token.substr(0, fldsplit))
      return std::make_pair("Private-token", token.substr(fldsplit + 1));
    warn("Unrecognized GitLab token type %s", token.substr(0, fldsplit));
    return std::make_pair(token.substr(0, fldsplit), token.substr(fldsplit + 1));
  }

  ref_info_t get_rev_from_ref(const settings_t& settings, nix::store_t& store,
                              const input_t& input) const override {
    auto host = maybe_get_str_attr(input.attrs, "host").value_or("gitlab.com");
    // See rate limiting note below
    auto url =
        fmt("https://%s/api/v4/projects/%s%%2F%s/repository/commits?ref_name=%s", host,
            get_str_attr(input.attrs, "owner"), get_str_attr(input.attrs, "repo"), *input.getRef());

    headers_t headers = make_headers_with_auth_tokens(settings, host, input);

    auto downloadResult = download_file(store, settings, url, "source", headers);
    auto json = nlohmann::json::parse(
        store.requireStoreObjectAccessor(downloadResult.store_path)->read_file(canon_path_t::root));

    if (json.is_array() && json.size() >= 1 && json[0]["id"] != nullptr) {
      return ref_info_t{.rev = Hash::parse_any(std::string(json[0]["id"]), hash_algorithm_t::SHA1)};
    }
    if (json.is_array() && json.size() == 0) {
      throw Error("No commits returned by GitLab API -- does the git ref really exist?");
    } else {
      throw Error("Unexpected response received from GitLab: %s", json);
    }
  }

  download_url_t get_download_url(const settings_t& settings, const input_t& input) const override {
    // This endpoint has a rate limit threshold that may be
    // server-specific and vary based whether the user is
    // authenticated via an accessToken or not, but the usual rate
    // is 10 reqs/sec/ip-addr.  See
    // https://docs.gitlab.com/ee/user/gitlab_com/index.html#gitlabcom-specific-rate-limits
    auto host = maybe_get_str_attr(input.attrs, "host").value_or("gitlab.com");
    auto url = fmt("https://%s/api/v4/projects/%s%%2F%s/repository/archive.tar.gz?sha=%s", host,
                   get_str_attr(input.attrs, "owner"), get_str_attr(input.attrs, "repo"),
                   input.getRev()->git_rev());

    headers_t headers = make_headers_with_auth_tokens(settings, host, input);
    return download_url_t{parse_url(url), headers};
  }

  void clone(const settings_t& settings, store_t& store, const input_t& input,
             const std::filesystem::path& dest_dir) const override {
    auto host = maybe_get_str_attr(input.attrs, "host").value_or("gitlab.com");
    // FIXME: get username somewhere
    input_t::fromURL(settings,
                     fmt("git+https://%s/%s/%s.git", host, get_str_attr(input.attrs, "owner"),
                         get_str_attr(input.attrs, "repo")))
        .applyOverrides(input.getRef(), input.getRev())
        .clone(settings, store, dest_dir);
  }
};

struct source_hut_input_scheme_t : git_archive_input_scheme_t {
  std::string_view schemeName() const override { return "sourcehut"; }

  std::string schemeDescription() const override {
    // TODO
    return "";
  }

  std::optional<std::pair<std::string, std::string>>
  accessHeaderFromToken(const std::string& token) const override {
    // SourceHut supports both PAT and OAuth2. See
    // https://man.sr.ht/meta.sr.ht/oauth.md
    return std::pair<std::string, std::string>("Authorization", fmt("Bearer %s", token));
    // Note: This currently serves no purpose, as this kind of authorization
    // does not allow for downloading tarballs on sourcehut private repos.
    // Once it is implemented, however, should work as expected.
  }

  ref_info_t get_rev_from_ref(const settings_t& settings, nix::store_t& store,
                              const input_t& input) const override {
    // TODO: In the future, when the sourcehut graphql API is implemented for mercurial
    // and with anonymous access, this method should use it instead.

    auto ref = *input.getRef();

    auto host = maybe_get_str_attr(input.attrs, "host").value_or("git.sr.ht");
    auto base_url = fmt("https://%s/%s/%s", host, get_str_attr(input.attrs, "owner"),
                        get_str_attr(input.attrs, "repo"));

    headers_t headers = make_headers_with_auth_tokens(settings, host, input);

    std::string refUri;
    if (ref == "HEAD") {
      auto downloadFileResult =
          download_file(store, settings, fmt("%s/HEAD", base_url), "source", headers);
      auto contents = store.requireStoreObjectAccessor(downloadFileResult.store_path)
                          ->read_file(canon_path_t::root);

      auto remoteLine = git::parse_ls_remote_line(get_line(contents).first);
      if (!remoteLine) {
        throw BadURL("in '%d', couldn't resolve HEAD ref '%d'", input.to_string(), ref);
      }
      refUri = remoteLine->target;
    } else {
      refUri = fmt("refs/(heads|tags)/%s", ref);
    }
    std::regex ref_regex(refUri);

    auto downloadFileResult =
        download_file(store, settings, fmt("%s/info/refs", base_url), "source", headers);
    auto contents = store.requireStoreObjectAccessor(downloadFileResult.store_path)
                        ->read_file(canon_path_t::root);
    std::istringstream is(contents);

    std::string line;
    std::optional<std::string> id;
    while (!id && getline(is, line)) {
      auto parsedLine = git::parse_ls_remote_line(line);
      if (parsedLine && parsedLine->reference &&
          std::regex_match(*parsedLine->reference, ref_regex))
        id = parsedLine->target;
    }

    if (!id)
      throw BadURL("in '%d', couldn't find ref '%d'", input.to_string(), ref);

    return ref_info_t{.rev = Hash::parse_any(*id, hash_algorithm_t::SHA1)};
  }

  download_url_t get_download_url(const settings_t& settings, const input_t& input) const override {
    auto host = maybe_get_str_attr(input.attrs, "host").value_or("git.sr.ht");
    auto url = fmt("https://%s/%s/%s/archive/%s.tar.gz", host, get_str_attr(input.attrs, "owner"),
                   get_str_attr(input.attrs, "repo"), input.getRev()->git_rev());

    headers_t headers = make_headers_with_auth_tokens(settings, host, input);
    return download_url_t{parse_url(url), headers};
  }

  void clone(const settings_t& settings, store_t& store, const input_t& input,
             const std::filesystem::path& dest_dir) const override {
    auto host = maybe_get_str_attr(input.attrs, "host").value_or("git.sr.ht");
    input_t::fromURL(settings, fmt("git+https://%s/%s/%s", host, get_str_attr(input.attrs, "owner"),
                                   get_str_attr(input.attrs, "repo")))
        .applyOverrides(input.getRef(), input.getRev())
        .clone(settings, store, dest_dir);
  }
};

static auto r_git_hub_input_scheme =
    on_startup_t([] { register_input_scheme(std::make_unique<git_hub_input_scheme_t>()); });
static auto r_git_lab_input_scheme =
    on_startup_t([] { register_input_scheme(std::make_unique<git_lab_input_scheme_t>()); });
static auto r_source_hut_input_scheme =
    on_startup_t([] { register_input_scheme(std::make_unique<source_hut_input_scheme_t>()); });

} // namespace nix::fetchers
