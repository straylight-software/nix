#include "nix/util/git.h"

#include <regex>

#include <string.h>
#include <sys/time.h>

#include "nix/fetchers/cache.h"
#include "nix/fetchers/fetch-settings.h"
#include "nix/fetchers/fetch-to-store.h"
#include "nix/fetchers/fetchers.h"
#include "nix/fetchers/git-utils.h"
#include "nix/store/globals.h"
#include "nix/store/pathlocks.h"
#include "nix/store/store-api.h"
#include "nix/util/archive.h"
#include "nix/util/error.h"
#include "nix/util/finally.h"
#include "nix/util/json-utils.h"
#include "nix/util/logging.h"
#include "nix/util/mounted-source-accessor.h"
#include "nix/util/processes.h"
#include "nix/util/tarfile.h"
#include "nix/util/url-parts.h"
#include "nix/util/users.h"

#ifndef _WIN32
#  include <sys/wait.h>
#endif

using namespace std::string_literals;

namespace nix::fetchers {

namespace {

// Explicit initial branch of our bare repo to suppress warnings from new version of git.
// The value itself does not matter, since we always fetch a specific revision or branch.
// It is set with `-c init.defaultBranch=` instead of `--initial-branch=` to stay compatible with
// old version of git, which will ignore unrecognized `-c` options.
const std::string git_initial_branch = "__nix_dummy_branch";

bool is_cache_file_within_ttl(time_t now, const struct stat& st) {
  return st.st_mtime + static_cast<time_t>(settings.tarballTtl) > now;
}

std::filesystem::path get_cache_path(std::string_view key, bool shallow) {
  return get_cache_dir() / "gitv3" /
         (hash_string(hash_algorithm_t::SHA256, key).to_string(hash_format_t::nix32, false) +
          (shallow ? "-shallow" : ""));
}

// Returns the name of the HEAD branch.
//
// Returns the head branch name as reported by git ls-remote --symref, e.g., if
// ls-remote returns the output below, "main" is returned based on the ref line.
//
//   ref: refs/heads/main       HEAD
//   ...
std::optional<std::string> read_head(const std::filesystem::path& path) {
  auto [status, output] = run_program(run_options_t{
      .program = "git",
      // FIXME: use 'HEAD' to avoid returning all refs
      .args = {"ls-remote", "--symref", path.string()},
      .is_interactive = true,
  });
  if (status != 0)
    return std::nullopt;

  std::string_view line = output;
  line = line.substr(0, line.find("\n"));
  if (const auto parse_result = git::parse_ls_remote_line(line);
      parse_result && parse_result->reference == "HEAD") {
    switch (parse_result->kind) {
      case git::ls_remote_ref_line_t::Kind::symbolic:
        debug("resolved HEAD ref '%s' for repo '%s'", parse_result->target, path);
        break;
      case git::ls_remote_ref_line_t::Kind::Object:
        debug("resolved HEAD rev '%s' for repo '%s'", parse_result->target, path);
        break;
    }
    return parse_result->target;
  }
  return std::nullopt;
}

// Persist the HEAD ref from the remote repo in the local cached repo.
bool store_cached_head(const std::string& actual_url, bool shallow, const std::string& head_ref) {
  std::filesystem::path cache_dir = get_cache_path(actual_url, shallow);
  try {
    run_program("git", true,
               {"-C", cache_dir.string(), "--git-dir", ".", "symbolic-ref", "--", "HEAD", head_ref});
  } catch (exec_error_t& e) {
    if (
#ifndef WIN32 // TODO abstract over exit status handling on Windows
        !WIFEXITED(e.status)
#else
        e.status != 0
#endif
    )
      throw;

    return false;
  }
  /* No need to touch refs/HEAD, because `git symbolic-ref` updates the mtime. */
  return true;
}

std::optional<std::string> read_head_cached(const std::string& actual_url, bool shallow) {
  // Create a cache path to store the branch of the HEAD ref. Append something
  // in front of the URL to prevent collision with the repository itself.
  std::filesystem::path cache_dir = get_cache_path(actual_url, shallow);
  std::filesystem::path head_ref_file = cache_dir / "HEAD";

  time_t now = time(0);
  struct stat st;
  std::optional<std::string> cachedRef;
  if (stat(head_ref_file.string().c_str(), &st) == 0) {
    cachedRef = read_head(cache_dir);
    if (cachedRef != std::nullopt && *cachedRef != git_initial_branch &&
        is_cache_file_within_ttl(now, st)) {
      debug("using cached HEAD ref '%s' for repo '%s'", *cachedRef, actual_url);
      return cachedRef;
    }
  }

  auto ref = read_head(actual_url);
  if (ref)
    return ref;

  if (cachedRef) {
    // If the cached git ref is expired in fetch() below, and the 'git fetch'
    // fails, it falls back to continuing with the most recent version.
    // This function must behave the same way, so we return the expired
    // cached ref here.
    warn("could not get HEAD ref for repository '%s'; using expired cached ref '%s'", actual_url,
         *cachedRef);
    return *cachedRef;
  }

  return std::nullopt;
}

std::vector<public_key_t> get_public_keys(const Attrs& attrs) {
  std::vector<public_key_t> public_keys;
  if (attrs.contains("publicKeys")) {
    auto pub_keys_json = nlohmann::json::parse(get_str_attr(attrs, "publicKeys"));
    auto& pub_keys = get_array(pub_keys_json);

    for (auto& key : pub_keys) {
      public_keys.push_back(key);
    }
  }
  if (attrs.contains("publicKey"))
    public_keys.push_back(public_key_t{maybe_get_str_attr(attrs, "keytype").value_or("ssh-ed25519"),
                                   get_str_attr(attrs, "publicKey")});
  return public_keys;
}

} // end namespace

static const Hash null_rev{hash_algorithm_t::SHA1};

struct git_input_scheme_t : InputScheme {
  std::optional<Input> inputFromURL(const settings_t& settings, const parsed_url_t& url,
                                    bool require_tree) const override {
    if (url.scheme != "git" && parse_url_scheme(url.scheme).application != "git")
      return {};

    auto url2(url);
    url2.query.clear();

    Attrs attrs;
    attrs.emplace("type", "git");

    for (auto& [name, value] : url.query) {
      if (name == "rev" || name == "ref" || name == "keytype" || name == "publicKey" ||
          name == "publicKeys")
        attrs.emplace(name, value);
      else if (name == "shallow" || name == "submodules" || name == "lfs" ||
               name == "exportIgnore" || name == "allRefs" || name == "verifyCommit")
        attrs.emplace(name, Explicit<bool>{value == "1"});
      else
        url2.query.emplace(name, value);
    }

    attrs.emplace("url", url2.to_string());

    return inputFromAttrs(settings, attrs);
  }

  std::string_view schemeName() const override { return "git"; }

  std::string schemeDescription() const override {
    return strip_indentation(R"(
          Fetch a git tree and copy it to the Nix store.
          This is similar to [`builtins.fetchGit`](@docroot@/language/builtins.md#builtins-fetchGit).
        )");
  }

  const std::map<std::string, AttributeInfo>& allowed_attrs() const override {
    static const std::map<std::string, AttributeInfo> attrs = {
        {
            "url",
            {
                .type = "String",
                .required = true,
                .doc = R"(
                      The URL formats supported are the same as for git itself.

                      > **Example**
                      >
                      > ```nix
                      > fetch_tree {
                      >   type = "git";
                      >   url = "git@github.com:NixOS/nixpkgs.git";
                      > }
                      > ```

                      > **Note**
                      >
                      > If the URL points to a local directory, and no `ref` or `rev` is given, Nix only considers files added to the git index, as listed by `git ls-files` but uses the *current file contents* of the git working directory.
                    )",
            },
        },
        {
            "ref",
            {
                .type = "String",
                .required = false,
                .doc = R"(
                      By default, this has no effect. This becomes relevant only once `shallow` cloning is disabled.

                      A [git reference](https://git-scm.com/book/en/v2/Git-Internals-Git-References), such as a branch or tag name.

                      Default: `"HEAD"`
                    )",
            },
        },
        {
            "rev",
            {
                .type = "String",
                .required = false,
                .doc = R"(
                      A git revision; a commit hash.

                      Default: the tip of `ref`
                    )",
            },
        },
        {
            "shallow",
            {
                .type = "Bool",
                .required = false,
                .doc = R"(
                      Make a shallow clone when fetching the git tree.
                      When this is enabled, the options `ref` and `allRefs` have no effect anymore.

                      Default: `true`
                    )",
            },
        },
        {
            "submodules",
            {
                .type = "Bool",
                .required = false,
                .doc = R"(
                      Also fetch submodules if available.

                      Default: `false`
                    )",
            },
        },
        {
            "lfs",
            {
                .type = "Bool",
                .required = false,
                .doc = R"(
                      Fetch any [git LFS](https://git-lfs.com/) files.

                      Default: `false`
                    )",
            },
        },
        {
            "exportIgnore",
            {},
        },
        {
            "lastModified",
            {
                .type = "Integer",
                .required = false,
                .doc = R"(
                      Unix timestamp of the fetched commit.

                      If set, pass through the value to the output attribute set.
                      Otherwise, generated from the fetched git tree.
                    )",
            },
        },
        {
            "revCount",
            {
                .type = "Integer",
                .required = false,
                .doc = R"(
                      Number of revisions in the history of the git repository before the fetched commit.

                      If set, pass through the value to the output attribute set.
                      Otherwise, generated from the fetched git tree.
                    )",
            },
        },
        {
            "narHash",
            {},
        },
        {
            "allRefs",
            {
                .type = "Bool",
                .required = false,
                .doc = R"(
                      By default, this has no effect. This becomes relevant only once `shallow` cloning is disabled.

                      Whether to fetch all references (eg. branches and tags) of the repository.
                      With this argument being true, it's possible to load a `rev` from *any* `ref`.
                      (Without setting this option, only `rev`s from the specified `ref` are supported).

                      Default: `false`
                    )",
            },
        },
        {
            "name",
            {},
        },
        {
            "dirtyRev",
            {},
        },
        {
            "dirtyShortRev",
            {},
        },
        {
            "verifyCommit",
            {},
        },
        {
            "keytype",
            {},
        },
        {
            "publicKey",
            {},
        },
        {
            "publicKeys",
            {},
        },
    };
    return attrs;
  }

  std::optional<Input> inputFromAttrs(const settings_t& settings, const Attrs& attrs) const override {
    for (auto& [name, _] : attrs)
      if (name == "verifyCommit" || name == "keytype" || name == "publicKey" ||
          name == "publicKeys")
        experimental_feature_settings.require(xp_t::verified_fetches);

    maybe_get_bool_attr(attrs, "verifyCommit");

    if (auto ref = maybe_get_str_attr(attrs, "ref"); ref && !is_legal_ref_name(*ref))
      throw BadURL("invalid Git branch/tag name '%s'", *ref);

    Input input{};
    input.attrs = attrs;
    input.attrs["url"] = fix_git_url(get_str_attr(attrs, "url")).to_string();
    get_shallow_attr(input);
    get_submodules_attr(input);
    get_all_refs_attr(input);
    return input;
  }

  parsed_url_t toURL(const Input& input, bool abbreviate) const override {
    auto url = parse_url(get_str_attr(input.attrs, "url"));
    if (url.scheme != "git")
      url.scheme = "git+" + url.scheme;
    if (auto rev = input.getRev())
      url.query.insert_or_assign("rev", rev->git_rev());
    if (auto ref = input.getRef()) {
      if (!abbreviate || (*ref != "master" && *ref != "main"))
        url.query.insert_or_assign("ref", *ref);
    }
    if (get_shallow_attr(input))
      url.query.insert_or_assign("shallow", "1");
    if (get_lfs_attr(input))
      url.query.insert_or_assign("lfs", "1");
    if (get_submodules_attr(input))
      url.query.insert_or_assign("submodules", "1");
    if (maybe_get_bool_attr(input.attrs, "exportIgnore").value_or(false))
      url.query.insert_or_assign("exportIgnore", "1");
    if (maybe_get_bool_attr(input.attrs, "verifyCommit").value_or(false))
      url.query.insert_or_assign("verifyCommit", "1");
    auto public_keys = get_public_keys(input.attrs);
    if (public_keys.size() == 1) {
      url.query.insert_or_assign("keytype", public_keys.at(0).type);
      url.query.insert_or_assign("publicKey", public_keys.at(0).key);
    } else if (public_keys.size() > 1)
      url.query.insert_or_assign("publicKeys", public_keys_to_string(public_keys));
    return url;
  }

  Input applyOverrides(const Input& input, std::optional<std::string> ref,
                       std::optional<Hash> rev) const override {
    auto res(input);
    if (rev)
      res.attrs.insert_or_assign("rev", rev->git_rev());
    if (ref)
      res.attrs.insert_or_assign("ref", *ref);
    if (!res.getRef() && res.getRev())
      throw Error("Git input '%s' has a commit hash but no branch/tag name", res.to_string());
    return res;
  }

  void clone(const settings_t& settings, Store& store, const Input& input,
             const std::filesystem::path& dest_dir) const override {
    auto repo_info = get_repo_info(input);

    strings_t args = {"clone"};

    args.push_back(repo_info.location_to_arg());

    if (auto ref = input.getRef()) {
      args.push_back("--branch");
      args.push_back(*ref);
    }

    if (input.getRev())
      throw UnimplementedError("cloning a specific revision is not implemented");

    args.push_back(dest_dir.string());

    run_program("git", true, args, {}, true);
  }

  std::optional<std::filesystem::path> get_source_path(const Input& input) const override {
    return get_repo_info(input).get_path();
  }

  void putFile(const Input& input, const canon_path_t& path, std::string_view contents,
               std::optional<std::string> commit_msg) const override {
    auto repo_info = get_repo_info(input);
    auto repo_path = repo_info.get_path();
    if (!repo_path)
      throw Error("cannot commit '%s' to Git repository '%s' because it's not a working tree", path,
                  input.to_string());

    write_file(*repo_path / path.rel(), contents);

    auto result = run_program(run_options_t{
        .program = "git",
        .args = {"-C", repo_path->string(), "--git-dir", repo_info.git_dir, "check-ignore", "--quiet",
                 std::string(path.rel())},
    });
    auto exit_code =
#ifndef WIN32 // TODO abstract over exit status handling on Windows
        WEXITSTATUS(result.first)
#else
        result.first
#endif
        ;

    if (exit_code != 0) {
      // The path is not `.gitignore`d, we can add the file.
      run_program("git", true,
                 {"-C", repo_path->string(), "--git-dir", repo_info.git_dir, "add", "--intent-to-add",
                  "--", std::string(path.rel())});

      if (commit_msg) {
        // Pause the logger to allow for user input (such as a gpg passphrase) in `git commit`
        auto suspension = logger->suspend();
        run_program("git", true,
                   {"-C", repo_path->string(), "--git-dir", repo_info.git_dir, "commit",
                    std::string(path.rel()), "-F", "-"},
                   *commit_msg);
      }
    }
  }

  struct repo_info_t {
    /* Either the path of the repo (for local, non-bare repos), or
       the URL (which is never a `file` URL). */
    std::variant<std::filesystem::path, parsed_url_t> location;

    /* Working directory info: the complete list of files, and
       whether the working directory is dirty compared to HEAD. */
    GitRepo::WorkdirInfo workdir_info;

    std::string location_to_arg() const {
      return std::visit(overloaded{[&](const std::filesystem::path& path) { return path.string(); },
                                   [&](const parsed_url_t& url) { return url.to_string(); }},
                        location);
    }

    std::optional<std::filesystem::path> get_path() const {
      if (auto path = std::get_if<std::filesystem::path>(&location))
        return *path;
      else
        return std::nullopt;
    }

    void warn_dirty(const settings_t& settings) const {
      if (workdir_info.isDirty) {
        if (!settings.allowDirty)
          throw Error("Git tree '%s' has uncommitted changes", location_to_arg());

        if (settings.warn_dirty)
          warn("Git tree '%s' has uncommitted changes", location_to_arg());
      }
    }

    std::string git_dir = ".git";
  };

  bool get_shallow_attr(const Input& input) const {
    return maybe_get_bool_attr(input.attrs, "shallow").value_or(false);
  }

  bool get_submodules_attr(const Input& input) const {
    return maybe_get_bool_attr(input.attrs, "submodules").value_or(false);
  }

  bool get_lfs_attr(const Input& input) const {
    return maybe_get_bool_attr(input.attrs, "lfs").value_or(false);
  }

  bool get_export_ignore_attr(const Input& input) const {
    return maybe_get_bool_attr(input.attrs, "exportIgnore").value_or(false);
  }

  bool get_all_refs_attr(const Input& input) const {
    return maybe_get_bool_attr(input.attrs, "allRefs").value_or(false);
  }

  repo_info_t get_repo_info(const Input& input) const {
    auto check_hash_algorithm = [&](const std::optional<Hash>& hash) {
      if (hash.has_value() &&
          !(hash->algo == hash_algorithm_t::SHA1 || hash->algo == hash_algorithm_t::SHA256))
        throw Error("Hash '%s' is not supported by Git. Supported types are sha1 and sha256.",
                    hash->to_string(hash_format_t::base16, true));
    };

    if (auto rev = input.getRev())
      check_hash_algorithm(rev);

    repo_info_t repo_info;

    // file:// URIs are normally not cloned (but otherwise treated the
    // same as remote URIs, i.e. we don't use the working tree or
    // HEAD). Exception: If _NIX_FORCE_HTTP is set, or the repo is a bare git
    // repo, treat as a remote URI to force a clone.
    static bool force_http = get_env("_NIX_FORCE_HTTP") == "1"; // for testing
    auto url = parse_url(get_str_attr(input.attrs, "url"));

    // Why are we checking for bare repository?
    // well if it's a bare repository we want to force a git fetch rather than copying the folder
    auto is_bare_repository = [](path_view_t path) {
      return path_exists(path) && !path_exists(path + "/.git");
    };

    // FIXME: here we turn a possibly relative path into an absolute path.
    // This allows relative git flake inputs to be resolved against the
    // **current working directory** (as in POSIX), which tends to work out
    // ok in the context of flakes, but is the wrong behavior,
    // as it should resolve against the flake.nix base directory instead.
    //
    // See: https://discourse.nixos.org/t/57783 and #9708
    //
    if (url.scheme == "file" && !force_http &&
        !is_bare_repository(render_url_path_ensure_legal(url.path))) {
      auto path = render_url_path_ensure_legal(url.path);

      if (!is_absolute(path)) {
        warn("Fetching Git repository '%s', which uses a path relative to the current directory. "
             "This is not supported and will stop working in a future release. "
             "See https://github.com/NixOS/nix/issues/12281 for details.",
             url);
      }

      repo_info.location = std::filesystem::absolute(path);
    } else {
      if (url.scheme == "file")
        /* Query parameters are meaningless for file://, but
           git interprets them as part of the file name. So get
           rid of them. */
        url.query.clear();
      /* Backward compatibility hack: In old versions of Nix, if you had
         a flake input like

           inputs.foo.url = "git+https://foo/bar?dir=subdir";

         it would result in a lock file entry like

           "original": {
             "dir": "subdir",
             "type": "git",
             "url": "https://foo/bar?dir=subdir"
           }

         New versions of Nix remove `?dir=subdir` from the `url` field,
         since the subdirectory is intended for `FlakeRef`, not the
         fetcher (and specifically the remote server), that is, the
         flakeref is parsed into

           "original": {
             "dir": "subdir",
             "type": "git",
             "url": "https://foo/bar"
           }

         However, new versions of nix parsing old flake.lock files would pass the dir=
         query parameter in the "url" attribute to git, which will then complain.

         For this reason, we are filtering the `dir` query parameter from the URL
         before passing it to git. */
      url.query.erase("dir");
      repo_info.location = url;
    }

    // If this is a local directory and no ref or revision is
    // given, then allow the use of an unclean working tree.
    if (auto repo_path = repo_info.get_path(); !input.getRef() && !input.getRev() && repo_path)
      repo_info.workdir_info = GitRepo::getCachedWorkdirInfo(*repo_path);

    return repo_info;
  }

  uint64_t get_last_modified(const settings_t& settings, const repo_info_t& repo_info,
                           const std::filesystem::path& repo_dir, const Hash& rev) const {
    cache_t::Key key{"gitLastModified", {{"rev", rev.git_rev()}}};

    auto cache = settings.get_cache();

    if (auto res = cache->lookup(key))
      return get_int_attr(*res, "lastModified");

    auto last_modified = GitRepo::openRepo(repo_dir, {})->get_last_modified(rev);

    cache->upsert(key, {{"lastModified", last_modified}});

    return last_modified;
  }

  uint64_t get_rev_count(const settings_t& settings, const repo_info_t& repo_info,
                       const std::filesystem::path& repo_dir, const Hash& rev) const {
    cache_t::Key key{"gitRevCount", {{"rev", rev.git_rev()}}};

    auto cache = settings.get_cache();

    if (auto revCountAttrs = cache->lookup(key))
      return get_int_attr(*revCountAttrs, "revCount");

    activity_t act(*logger, lvl_chatty, act_unknown,
                 fmt("getting Git revision count of '%s'", repo_info.location_to_arg()));

    auto rev_count = GitRepo::openRepo(repo_dir, {})->get_rev_count(rev);

    cache->upsert(key, Attrs{{"revCount", rev_count}});

    return rev_count;
  }

  std::string get_default_ref(const repo_info_t& repo_info, bool shallow) const {
    auto head = std::visit(
        overloaded{[&](const std::filesystem::path& path) {
                     return GitRepo::openRepo(path, {})->getWorkdirRef();
                   },
                   [&](const parsed_url_t& url) { return read_head_cached(url.to_string(), shallow); }},
        repo_info.location);
    if (!head) {
      warn("could not read HEAD ref from repo at '%s', using 'master'", repo_info.location_to_arg());
      return "master";
    }
    return *head;
  }

  static MakeNotAllowedError make_not_allowed_error(std::filesystem::path repo_path) {
    return [repo_path{std::move(repo_path)}](const canon_path_t& path) -> RestrictedPathError {
      if (path_exists(repo_path / path.rel()))
        return RestrictedPathError("Path '%1%' in the repository %2% is not tracked by Git.\n"
                                   "\n"
                                   "To make it visible to Nix, run:\n"
                                   "\n"
                                   "git -C %2% add \"%1%\"",
                                   path.rel(), repo_path);
      else
        return RestrictedPathError("Path '%s' does not exist in Git repository %s.", path.rel(),
                                   repo_path);
    };
  }

  void verify_commit(const Input& input, std::shared_ptr<GitRepo> repo) const {
    auto public_keys = get_public_keys(input.attrs);
    auto verify_commit = maybe_get_bool_attr(input.attrs, "verifyCommit").value_or(!public_keys.empty());

    if (verify_commit) {
      if (input.getRev() && repo)
        repo->verify_commit(*input.getRev(), public_keys);
      else
        throw Error("commit verification is required for Git repository '%s', but it's dirty",
                    input.to_string());
    }
  }

  /**
   * Decide whether we can do a shallow clone, which is faster. This is possible if the user
   * explicitly specified `shallow = true`, or if we already have a `rev_count`.
   */
  bool can_do_shallow(const Input& input) const {
    bool shallow = get_shallow_attr(input);
    return shallow || input.get_rev_count().has_value();
  }

  GitAccessorOptions get_git_accessor_options(const Input& input) const {
    return GitAccessorOptions{
        .export_ignore = get_export_ignore_attr(input),
        .smudgeLfs = get_lfs_attr(input),
        .submodules = get_submodules_attr(input),
    };
  }

  /**
   * Get a `SourceAccessor` for the given git revision using Nix < 2.20 semantics, i.e. using `git
   * archive` or `git checkout`.
   */
  ref<SourceAccessor> get_legacy_git_accessor(Store& store, repo_info_t& repo_info,
                                           const std::filesystem::path& repo_dir, const Hash& rev,
                                           GitAccessorOptions& options) const {
    auto tmp_dir = create_temp_dir();
    auto_delete_t del_tmp_dir(tmp_dir, true);

    auto store_path =
        options.submodules ? [&]() {
          // Nix < 2.20 used `git checkout` for repos with submodules.
          run_program2({.program = "git", .args = {"init", tmp_dir}});
          run_program2(
              {.program = "git", .args = {"-C", tmp_dir, "remote", "add", "origin", repo_dir}});
          run_program2({.program = "git", .args = {"-C", tmp_dir, "fetch", "origin", rev.git_rev()}});
          run_program2({.program = "git", .args = {"-C", tmp_dir, "checkout", rev.git_rev()}});
          path_filter_t filter = [&](const Path& path) { return base_name_of(path) != ".git"; };
          return store.add_to_store("source", {get_fs_source_accessor(), canon_path_t(tmp_dir.string())},
                                  ContentAddressMethod::raw_t::nix_archive, hash_algorithm_t::SHA256, {},
                                  filter);
        }()
                           : [&]() {
                               // Nix < 2.20 used `git archive` for repos without submodules.
                               options.export_ignore = true;

                               auto source = sink_to_source([&](Sink& sink) {
                                 run_program2({.program = "git",
                                              .args = {"-C", repo_dir, "--git-dir", repo_info.git_dir,
                                                       "archive", rev.git_rev()},
                                              .standard_out = &sink});
                               });

                               unpack_tarfile(*source, tmp_dir);

                               return store.add_to_store(
                                   "source", {get_fs_source_accessor(), canon_path_t(tmp_dir.string())});
                             }();

    auto accessor = store.getFSAccessor(store_path);

    accessor->fingerprint = options.makeFingerprint(rev) + ";legacy";

    return ref{accessor};
  }

  std::pair<ref<SourceAccessor>, Input> get_accessor_from_commit(const settings_t& settings,
                                                              Store& store, repo_info_t& repo_info,
                                                              Input&& input) const {
    assert(!repo_info.workdir_info.isDirty);

    auto orig_rev = input.getRev();

    auto original_ref = input.getRef();
    bool shallow = can_do_shallow(input);
    auto ref = original_ref ? *original_ref : get_default_ref(repo_info, shallow);
    input.attrs.insert_or_assign("ref", ref);

    std::filesystem::path repo_dir;

    if (auto repo_path = repo_info.get_path()) {
      repo_dir = *repo_path;
      if (!input.getRev())
        input.attrs.insert_or_assign("rev",
                                     GitRepo::openRepo(repo_dir, {})->resolveRef(ref).git_rev());
    } else {
      auto rev = input.getRev();
      auto repo_url = std::get<parsed_url_t>(repo_info.location);
      std::filesystem::path cache_dir = get_cache_path(repo_url.to_string(), shallow);
      repo_dir = cache_dir;
      repo_info.git_dir = ".";

      /* If shallow = false, but we have a non-shallow repo that already contains the desired rev,
       * then use that repo instead. */
      std::filesystem::path cache_dir_non_shallow = get_cache_path(repo_url.to_string(), false);
      if (rev && shallow && path_exists(cache_dir_non_shallow)) {
        auto non_shallow_repo = GitRepo::openRepo(cache_dir_non_shallow, {.create = true, .bare = true});
        if (non_shallow_repo->hasObject(*rev)) {
          debug("using non-shallow cached repo for '%s' since it contains rev '%s'",
                repo_url.to_string(), rev->git_rev());
          repo_dir = cache_dir_non_shallow;
          goto have_rev;
        }
      }

      std::filesystem::create_directories(cache_dir.parent_path());
      PathLocks cache_dir_lock({cache_dir.string()});

      auto repo = GitRepo::openRepo(cache_dir, {.create = true, .bare = true});

      // We need to set the origin so resolving submodule URLs works
      repo->setRemote("origin", repo_url.to_string());

      auto local_ref_file =
          ref.compare(0, 5, "refs/") == 0 ? cache_dir / ref : cache_dir / "refs/heads" / ref;

      bool do_fetch = false;
      time_t now = time(0);

      /* If a rev was specified, we need to fetch if it's not in the
         repo. */
      if (rev) {
        do_fetch = !repo->hasObject(*rev);
      } else {
        if (get_all_refs_attr(input)) {
          do_fetch = true;
        } else {
          /* If the local ref is older than ‘tarball-ttl’ seconds, do a
             git fetch to update the local ref to the remote ref. */
          struct stat st;
          do_fetch = stat(local_ref_file.string().c_str(), &st) != 0 || !is_cache_file_within_ttl(now, st);
        }
      }

      if (do_fetch) {
        try {
          auto fetch_ref = get_all_refs_attr(input)             ? "refs/*:refs/*"
                          : input.getRev()                  ? input.getRev()->git_rev()
                          : ref.compare(0, 5, "refs/") == 0 ? fmt("%1%:%1%", ref)
                          : ref == "HEAD"                   ? "HEAD:HEAD"
                                                            : fmt("%1%:%1%", "refs/heads/" + ref);

          repo->fetch(repo_url.to_string(), fetch_ref, shallow);
        } catch (Error& e) {
          if (!std::filesystem::exists(local_ref_file))
            throw;
          logError(e.info());
          warn("could not update local clone of Git repository '%s'; continuing with the most "
               "recent version",
               repo_info.location_to_arg());
        }

        try {
          if (!input.getRev())
            set_write_time(local_ref_file, now, now);
        } catch (Error& e) {
          warn("could not update mtime for file %s: %s", local_ref_file, e.info().msg);
        }
        if (!original_ref && !store_cached_head(repo_url.to_string(), shallow, ref))
          warn("could not update cached head '%s' for '%s'", ref, repo_info.location_to_arg());
      }

      if (rev) {
        if (!repo->hasObject(*rev))
          throw Error("Cannot find Git revision '%s' in ref '%s' of repository '%s'! "
                      "Please make sure that the " ANSI_BOLD "rev" ANSI_NORMAL
                      " exists on the " ANSI_BOLD "ref" ANSI_NORMAL
                      " you've specified or add " ANSI_BOLD "allRefs = true;" ANSI_NORMAL
                      " to " ANSI_BOLD "fetchGit" ANSI_NORMAL ".",
                      rev->git_rev(), ref, repo_info.location_to_arg());
      } else
        input.attrs.insert_or_assign("rev", repo->resolveRef(ref).git_rev());

      // cache dir lock is removed at scope end; we will only use read-only operations on specific
      // revisions in the remainder
    }

  have_rev:
    auto repo = GitRepo::openRepo(repo_dir, {});

    // FIXME: check whether rev is an ancestor of ref?

    auto rev = *input.getRev();

    /* Skip last_modified computation if it's already supplied by the caller.
       We don't care if they specify an incorrect value; it doesn't
       matter for security, unlike nar_hash. */
    if (!input.attrs.contains("lastModified"))
      input.attrs.insert_or_assign("lastModified",
                                   get_last_modified(settings, repo_info, repo_dir, rev));

    /* Like last_modified, skip rev_count if supplied by the caller. */
    if (!shallow && !input.attrs.contains("revCount")) {
      auto is_shallow = repo->is_shallow();

      if (is_shallow && !shallow)
        throw Error("'%s' is a shallow Git repository, but shallow repositories are only allowed "
                    "when `shallow = true;` is specified",
                    repo_info.location_to_arg());

      input.attrs.insert_or_assign("revCount", get_rev_count(settings, repo_info, repo_dir, rev));
    }

    printTalkative("using revision %s of repo '%s'", rev.git_rev(), repo_info.location_to_arg());

    verify_commit(input, repo);

    auto options = get_git_accessor_options(input);

    auto expected_nar_hash = input.getNarHash();

    auto accessor = repo->get_accessor(rev, options, "«" + input.to_string(true) + "»");

    if (settings.nix219Compat && !options.smudgeLfs &&
        accessor->path_exists(canon_path_t(".gitattributes"))) {
      /* use Nix 2.19 semantics to generate locks, but if a NAR hash is specified, support Nix
       * >= 2.20 semantics as well. */
      warn("Using Nix 2.19 semantics to export Git repository '%s'.", input.to_string());
      auto accessor_modern = accessor;
      accessor = get_legacy_git_accessor(store, repo_info, repo_dir, rev, options);
      if (expected_nar_hash) {
        auto nar_hash_legacy =
            fetch_to_store2(settings, store, {accessor}, FetchMode::DryRun, input.get_name()).second;
        if (expected_nar_hash != nar_hash_legacy) {
          auto nar_hash_modern =
              fetch_to_store2(settings, store, {accessor_modern}, FetchMode::DryRun, input.get_name())
                  .second;
          if (expected_nar_hash == nar_hash_modern)
            accessor = accessor_modern;
        }
      }
    } else {
      /* Backward compatibility hack for locks produced by Nix < 2.20 that depend on Nix applying
       * git filters, `export-ignore` or `export-subst`. Nix >= 2.20 doesn't do those, so we may get
       * a NAR hash mismatch. If that happens, try again using `git archive`. */
      auto nar_hash_new =
          fetch_to_store2(settings, store, {accessor}, FetchMode::DryRun, input.get_name()).second;
      if (expected_nar_hash && accessor->path_exists(canon_path_t(".gitattributes"))) {
        if (expected_nar_hash != nar_hash_new) {
          auto accessor_legacy = get_legacy_git_accessor(store, repo_info, repo_dir, rev, options);
          auto nar_hash_legacy =
              fetch_to_store2(settings, store, {accessor_legacy}, FetchMode::DryRun, input.get_name())
                  .second;
          if (expected_nar_hash == nar_hash_legacy) {
            warn("Git input '%s' specifies a NAR hash '%s' that was created by Nix < 2.20.\n"
                 "Nix >= 2.20 does not apply Git filters, `export-ignore` and `export-subst` by "
                 "default, which changes the NAR hash.\n"
                 "Please update the NAR hash to '%s'.",
                 input.to_string(), expected_nar_hash->to_string(hash_format_t::SRI, true),
                 nar_hash_new.to_string(hash_format_t::SRI, true));
            accessor = accessor_legacy;
          }
        }
      }
    }

    /* If the repo has submodules, fetch them and return a mounted
       input accessor consisting of the accessor for the top-level
       repo and the accessors for the submodules. */
    if (options.submodules) {
      std::map<canon_path_t, nix::ref<SourceAccessor>> mounts;

      for (auto& [submodule, submoduleRev] : repo->getSubmodules(rev, options.export_ignore)) {
        auto resolved = repo->resolveSubmoduleUrl(submodule.url);
        debug("Git submodule %s: %s %s %s -> %s", submodule.path, submodule.url, submodule.branch,
              submoduleRev.git_rev(), resolved);
        fetchers::Attrs attrs;
        attrs.insert_or_assign("type", "git");
        attrs.insert_or_assign("url", resolved);
        if (submodule.branch != "") {
          // A special value of . is used to indicate that the name of the branch in the submodule
          // should be the same name as the current branch in the current repository.
          // https://git-scm.com/docs/gitmodules
          if (submodule.branch == ".") {
            attrs.insert_or_assign("ref", ref);
          } else {
            attrs.insert_or_assign("ref", submodule.branch);
          }
        }
        attrs.insert_or_assign("rev", submoduleRev.git_rev());
        attrs.insert_or_assign("exportIgnore", Explicit<bool>{options.export_ignore});
        attrs.insert_or_assign("submodules", Explicit<bool>{true});
        attrs.insert_or_assign("lfs", Explicit<bool>{options.smudgeLfs});
        attrs.insert_or_assign("allRefs", Explicit<bool>{true});
        auto submoduleInput = fetchers::Input::fromAttrs(settings, std::move(attrs));
        auto [submoduleAccessor, submoduleInput2] = submoduleInput.get_accessor(settings, store);
        submoduleAccessor->set_path_display("«" + submoduleInput.to_string(true) + "»");
        mounts.insert_or_assign(submodule.path, submoduleAccessor);
      }

      if (!mounts.empty()) {
        auto new_fingerprint = accessor->get_fingerprint(canon_path_t::root).second->append(";s");
        mounts.insert_or_assign(canon_path_t::root, accessor);
        accessor = make_mounted_source_accessor(std::move(mounts));
        accessor->fingerprint = new_fingerprint;
      }
    }

    assert(!orig_rev || orig_rev == rev);

    return {accessor, std::move(input)};
  }

  std::pair<ref<SourceAccessor>, Input> get_accessor_from_workdir(const settings_t& settings,
                                                               Store& store, repo_info_t& repo_info,
                                                               Input&& input) const {
    auto repo_path = repo_info.get_path().value();

    if (get_submodules_attr(input))
      /* Create mountpoints for the submodules. */
      for (auto& submodule : repo_info.workdir_info.submodules)
        repo_info.workdir_info.files.insert(submodule.path);

    auto repo = GitRepo::openRepo(repo_path, {});

    auto export_ignore = get_export_ignore_attr(input);

    ref<SourceAccessor> accessor = repo->get_accessor(
        repo_info.workdir_info, {.export_ignore = export_ignore}, make_not_allowed_error(repo_path));

    /* If the repo has submodules, return a mounted input accessor
       consisting of the accessor for the top-level repo and the
       accessors for the submodule workdirs. */
    if (get_submodules_attr(input) && !repo_info.workdir_info.submodules.empty()) {
      std::map<canon_path_t, nix::ref<SourceAccessor>> mounts;

      for (auto& submodule : repo_info.workdir_info.submodules) {
        auto submodulePath = repo_path / submodule.path.rel();
        fetchers::Attrs attrs;
        attrs.insert_or_assign("type", "git");
        attrs.insert_or_assign("url", submodulePath.string());
        attrs.insert_or_assign("exportIgnore", Explicit<bool>{export_ignore});
        attrs.insert_or_assign("submodules", Explicit<bool>{true});
        // TODO: fall back to getAccessorFromCommit-like fetch when submodules aren't checked out
        // attrs.insert_or_assign("allRefs", Explicit<bool>{ true });

        auto submoduleInput = fetchers::Input::fromAttrs(settings, std::move(attrs));
        auto [submoduleAccessor, submoduleInput2] = submoduleInput.get_accessor(settings, store);
        submoduleAccessor->set_path_display("«" + submoduleInput.to_string(true) + "»");

        /* If the submodule is dirty, mark this repo dirty as
           well. */
        if (!submoduleInput2.getRev())
          repo_info.workdir_info.isDirty = true;

        mounts.insert_or_assign(submodule.path, submoduleAccessor);
      }

      mounts.insert_or_assign(canon_path_t::root, accessor);
      accessor = make_mounted_source_accessor(std::move(mounts));
    }

    if (!repo_info.workdir_info.isDirty) {
      auto repo = GitRepo::openRepo(repo_path, {});

      if (auto ref = repo->getWorkdirRef())
        input.attrs.insert_or_assign("ref", *ref);

      /* Return a rev of 000... if there are no commits yet. */
      auto rev = repo_info.workdir_info.headRev.value_or(null_rev);

      input.attrs.insert_or_assign("rev", rev.git_rev());
      if (!get_shallow_attr(input)) {
        input.attrs.insert_or_assign(
            "revCount", rev == null_rev ? 0 : get_rev_count(settings, repo_info, repo_path, rev));
      }

      verify_commit(input, repo);
    } else {
      repo_info.warn_dirty(settings);

      if (repo_info.workdir_info.headRev) {
        input.attrs.insert_or_assign("dirtyRev", repo_info.workdir_info.headRev->git_rev() + "-dirty");
        input.attrs.insert_or_assign("dirtyShortRev",
                                     repo_info.workdir_info.headRev->git_short_rev() + "-dirty");
      }

      verify_commit(input, nullptr);
    }

    input.attrs.insert_or_assign(
        "lastModified",
        repo_info.workdir_info.headRev
            ? get_last_modified(settings, repo_info, repo_path, *repo_info.workdir_info.headRev)
            : 0);

    return {accessor, std::move(input)};
  }

  std::pair<ref<SourceAccessor>, Input> get_accessor(const settings_t& settings, Store& store,
                                                    const Input& _input) const override {
    Input input(_input);

    auto repo_info = get_repo_info(input);

    if (get_export_ignore_attr(input) && get_submodules_attr(input)) {
      /* In this situation, we don't have a git CLI behavior that we can copy.
         `git archive` does not support submodules, so it is unclear whether
         rules from the parent should affect the submodule or not.
         When git may eventually implement this, we need Nix to match its
         behavior. */
      throw UnimplementedError("exportIgnore and submodules are not supported together yet");
    }

    auto [accessor, final] =
        input.getRef() || input.getRev() || !repo_info.get_path()
            ? get_accessor_from_commit(settings, store, repo_info, std::move(input))
            : get_accessor_from_workdir(settings, store, repo_info, std::move(input));

    return {accessor, std::move(final)};
  }

  std::optional<std::string> get_fingerprint(Store& store, const Input& input) const override {
    auto options = get_git_accessor_options(input);

    if (auto rev = input.getRev())
      // FIXME: this can return a wrong fingerprint for the legacy (`git archive`) case, since we
      // don't know here whether to append the `;legacy` suffix or not.
      return options.makeFingerprint(*rev);
    else {
      auto repo_info = get_repo_info(input);
      if (auto repo_path = repo_info.get_path(); repo_path && repo_info.workdir_info.submodules.empty()) {
        /* Calculate a fingerprint that takes into account the
           deleted and modified/added files. */
        hash_sink_t hash_sink{hash_algorithm_t::SHA512};
        for (auto& file : repo_info.workdir_info.dirtyFiles) {
          write_string("modified:", hash_sink);
          write_string(file.abs(), hash_sink);
          dump_path((*repo_path / file.rel()).string(), hash_sink);
        }
        for (auto& file : repo_info.workdir_info.deletedFiles) {
          write_string("deleted:", hash_sink);
          write_string(file.abs(), hash_sink);
        }
        return options.makeFingerprint(repo_info.workdir_info.headRev.value_or(null_rev)) +
               ";d=" + hash_sink.finish().hash.to_string(hash_format_t::base16, false);
      }
      return std::nullopt;
    }
  }

  bool isLocked(const settings_t& settings, const Input& input) const override {
    auto rev = input.getRev();
    return rev && rev != null_rev;
  }
};

static auto r_git_input_scheme =
    on_startup_t([] { register_input_scheme(std::make_unique<git_input_scheme_t>()); });

} // namespace nix::fetchers
