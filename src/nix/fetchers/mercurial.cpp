#include <sys/time.h>

#include "nix/fetchers/cache.h"
#include "nix/fetchers/fetch-settings.h"
#include "nix/fetchers/fetchers.h"
#include "nix/store/globals.h"
#include "nix/store/store-api.h"
#include "nix/util/processes.h"
#include "nix/util/tarfile.h"
#include "nix/util/url-parts.h"
#include "nix/util/users.h"

namespace nix::fetchers {

static run_options_t hg_options(const strings_t& args) {
  auto env = get_env();
  // Set HGPLAIN: this means we get consistent output from hg and avoids leakage from a user or
  // system .hgrc.
  env["HGPLAIN"] = "";

  return {.program = "hg", .lookup_path = true, .args = args, .environment = env};
}

// runProgram wrapper that uses hgOptions instead of stock RunOptions.
static std::string run_hg(const strings_t& args, const std::optional<std::string>& input = {}) {
  run_options_t opts = hg_options(args);
  opts.input = input;

  auto res = run_program(std::move(opts));

  if (!status_ok(res.first)) {
    throw exec_error_t(res.first, "hg %1%", status_to_string(res.first));
  }

  return res.second;
}

struct mercurial_input_scheme_t : input_scheme_t {
  std::optional<input_t> inputFromURL(const settings_t& settings, const parsed_url_t& url,
                                      bool require_tree) const override {
    if (url.scheme() != "hg+http" && url.scheme() != "hg+https" && url.scheme() != "hg+ssh" &&
        url.scheme() != "hg+file") {
      return {};
    }

    auto url2(url);
    url2.set_scheme(std::string(url2.scheme(), 3));
    url2.query().clear();

    Attrs attrs;
    attrs.emplace("type", "hg");

    for (auto& [name, value] : url.query()) {
      if (name == "rev" || name == "ref") {
        attrs.emplace(name, value);
      } else {
        url2.query().emplace(name, value);
      }
    }

    attrs.emplace("url", url2.to_string());

    return inputFromAttrs(settings, attrs);
  }

  std::string_view schemeName() const override { return "hg"; }

  std::string schemeDescription() const override {
    // TODO
    return "";
  }

  const std::map<std::string, AttributeInfo>& allowed_attrs() const override {
    static const std::map<std::string, AttributeInfo> attrs = {
        {
            "url",
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
            "revCount",
            {},
        },
        {
            "narHash",
            {},
        },
        {
            "name",
            {},
        },
    };
    return attrs;
  }

  std::optional<input_t> inputFromAttrs(const settings_t& settings,
                                        const Attrs& attrs) const override {
    parse_url(get_str_attr(attrs, "url"));

    if (auto ref = maybe_get_str_attr(attrs, "ref")) {
      if (!std::regex_match(*ref, ref_regex)) {
        throw BadURL("invalid Mercurial branch/tag name '%s'", *ref);
      }
    }

    input_t input{};
    input.attrs = attrs;
    return input;
  }

  parsed_url_t toURL(const input_t& input, bool abbreviate) const override {
    auto url = parse_url(get_str_attr(input.attrs, "url"));
    url.set_scheme("hg+" + url.scheme());
    if (auto rev = input.getRev()) {
      url.query().insert_or_assign("rev", rev->git_rev());
    }
    if (auto ref = input.getRef()) {
      url.query().insert_or_assign("ref", *ref);
    }
    return url;
  }

  input_t applyOverrides(const input_t& input, std::optional<std::string> ref,
                         std::optional<Hash> rev) const override {
    auto res(input);
    if (rev) {
      res.attrs.insert_or_assign("rev", rev->git_rev());
    }
    if (ref) {
      res.attrs.insert_or_assign("ref", *ref);
    }
    return res;
  }

  std::optional<std::filesystem::path> get_source_path(const input_t& input) const override {
    auto url = parse_url(get_str_attr(input.attrs, "url"));
    if (url.scheme() == "file" && !input.getRef() && !input.getRev()) {
      return render_url_path_ensure_legal(url.path());
    }
    return {};
  }

  void putFile(const input_t& input, const canon_path_t& path, std::string_view contents,
               std::optional<std::string> commit_msg) const override {
    auto [is_local, repo_path] = get_actual_url(input);
    if (!is_local) {
      throw Error("cannot commit '%s' to Mercurial repository '%s' because it's not a working tree",
                  path, input.to_string());
    }

    auto abs_path = canon_path_t(repo_path) / path;

    write_file(abs_path.abs(), contents);

    // FIXME: shut up if file is already tracked.
    run_hg({"add", abs_path.abs()});

    if (commit_msg) {
      run_hg({"commit", abs_path.abs(), "-m", *commit_msg});
    }
  }

  std::pair<bool, std::string> get_actual_url(const input_t& input) const {
    auto url = parse_url(get_str_attr(input.attrs, "url"));
    bool is_local = url.scheme() == "file";
    return {is_local, is_local ? render_url_path_ensure_legal(url.path()) : url.to_string()};
  }

  store_path_t fetch_to_store(const settings_t& settings, store_t& store, input_t& input) const {
    auto orig_rev = input.getRev();

    auto name = input.get_name();

    auto [is_local, actualUrl_] = get_actual_url(input);
    auto actual_url = actualUrl_; // work around clang bug

    // FIXME: return lastModified.

    // FIXME: don't clone local repositories.

    if (!input.getRef() && !input.getRev() && is_local && path_exists(actual_url + "/.hg")) {
      bool clean = run_hg({"status", "-R", actual_url, "--modified", "--added", "--removed"}) == "";

      if (!clean) {
        /* This is an unclean working tree. So copy all tracked
           files. */

        if (!settings.allowDirty) {
          throw Error("Mercurial tree '%s' is unclean", actual_url);
        }

        if (settings.warn_dirty) {
          warn("Mercurial tree '%s' is unclean", actual_url);
        }

        input.attrs.insert_or_assign("ref", chomp(run_hg({"branch", "-R", actual_url})));

        auto files = tokenize_string<string_set_t>(
            run_hg({"status", "-R", actual_url, "--clean", "--modified", "--added", "--no-status",
                    "--print0"}),
            std::string("\0", 1));

        std::filesystem::path actualPath(abs_path(actual_url));

        path_filter_t filter = [&](const Path& p) -> bool {
          assert(has_prefix(p, actualPath.string()));
          std::string file(p, actualPath.string().size() + 1);

          auto st = lstat(p);

          if (S_ISDIR(st.st_mode)) {
            auto prefix = file + "/";
            auto i = files.lower_bound(prefix);
            return i != files.end() && has_prefix(*i, prefix);
          }

          return files.count(file);
        };

        auto store_path = store.add_to_store(
            input.get_name(), {get_fs_source_accessor(), canon_path_t(actualPath.string())},
            content_address_method_t::raw_t::nix_archive, hash_algorithm_t::SHA256, {}, filter);

        return store_path;
      }
    }

    if (!input.getRef()) {
      input.attrs.insert_or_assign("ref", "default");
    }

    auto rev_info_key = [&](const Hash& rev) {
      if (rev.algo() != hash_algorithm_t::SHA1) {
        throw Error("Hash '%s' is not supported by Mercurial. Only sha1 is supported.",
                    rev.git_rev());
      }

      return cache_t::Key{
          "hgRev",
          {{"store", store.store_dir}, {"name", name}, {"rev", input.getRev()->git_rev()}}};
    };

    auto make_result = [&](const Attrs& info_attrs,
                           const store_path_t& store_path) -> store_path_t {
      assert(input.getRev());
      assert(!orig_rev || orig_rev == input.getRev());
      input.attrs.insert_or_assign("revCount", get_int_attr(info_attrs, "revCount"));
      return store_path;
    };

    /* Check the cache for the most recent rev for this URL/ref. */
    cache_t::Key ref_to_rev_key{"hgRefToRev", {{"url", actual_url}, {"ref", *input.getRef()}}};

    if (!input.getRev()) {
      if (auto res = settings.get_cache()->lookupWithTTL(ref_to_rev_key)) {
        input.attrs.insert_or_assign("rev", get_rev_attr(*res, "rev").git_rev());
      }
    }

    /* If we have a rev, check if we have a cached store path. */
    if (auto rev = input.getRev()) {
      if (auto res = settings.get_cache()->lookupStorePath(rev_info_key(*rev), store)) {
        return make_result(res->value, res->store_path);
      }
    }

    std::filesystem::path cache_dir =
        get_cache_dir() / "hg" /
        hash_string(hash_algorithm_t::SHA256, actual_url).to_string(hash_format_t::nix32, false);

    /* If this is a commit hash that we already have, we don't
       have to pull again. */
    if (!(input.getRev() && path_exists(cache_dir) &&
          run_program(hg_options({"log", "-R", cache_dir.string(), "-r", input.getRev()->git_rev(),
                                  "--template", "1"}))
                  .second == "1")) {
      activity_t act(*logger, lvl_talkative, act_unknown,
                     fmt("fetching Mercurial repository '%s'", actual_url));

      if (path_exists(cache_dir)) {
        try {
          run_hg({"pull", "-R", cache_dir.string(), "--", actual_url});
        } catch (exec_error_t& e) {
          auto trans_journal = cache_dir / ".hg" / "store" / "journal";
          /* hg throws "abandoned transaction" error only if this file exists */
          if (path_exists(trans_journal)) {
            run_hg({"recover", "-R", cache_dir.string()});
            run_hg({"pull", "-R", cache_dir.string(), "--", actual_url});
          } else {
            throw exec_error_t(e.status, "'hg pull' %s", status_to_string(e.status));
          }
        }
      } else {
        create_dirs(dir_of(cache_dir.string()));
        run_hg({"clone", "--noupdate", "--", actual_url, cache_dir.string()});
      }
    }

    /* Fetch the remote rev or ref. */
    auto ref_or_rev = input.getRev() ? input.getRev()->git_rev() : *input.getRef();
    auto tokens = tokenize_string<std::vector<std::string>>(
        run_hg({"log", "-R", cache_dir.string(), "-r", ref_or_rev, "--template",
                "{node} {rev} {branch}"}));
    if (tokens.size() != 3) {
      throw Error("unexpected output from 'hg log' for ref '%s': expected 3 fields, got %d",
                  ref_or_rev, tokens.size());
    }

    auto rev = Hash::parse_any(tokens[0], hash_algorithm_t::SHA1);
    input.attrs.insert_or_assign("rev", rev.git_rev());
    auto rev_count = std::stoull(tokens[1]);
    input.attrs.insert_or_assign("ref", tokens[2]);

    /* Now that we have the rev, check the cache again for a
       cached store path. */
    if (auto res = settings.get_cache()->lookupStorePath(rev_info_key(rev), store)) {
      return make_result(res->value, res->store_path);
    }

    std::filesystem::path tmp_dir = create_temp_dir();
    auto_delete_t del_tmp_dir(tmp_dir, true);

    run_hg({"archive", "-R", cache_dir.string(), "-r", rev.git_rev(), tmp_dir.string()});

    delete_path(tmp_dir / ".hg_archival.txt");

    auto store_path =
        store.add_to_store(name, {get_fs_source_accessor(), canon_path_t(tmp_dir.string())});

    Attrs info_attrs({
        {"revCount", (uint64_t)rev_count},
    });

    if (!orig_rev) {
      settings.get_cache()->upsert(ref_to_rev_key, {{"rev", rev.git_rev()}});
    }

    settings.get_cache()->upsert(rev_info_key(rev), store, info_attrs, store_path);

    return make_result(info_attrs, std::move(store_path));
  }

  std::pair<ref<source_accessor_t>, input_t>
  get_accessor(const settings_t& settings, store_t& store, const input_t& _input) const override {
    input_t input(_input);

    auto store_path = fetch_to_store(settings, store, input);
    auto accessor = store.requireStoreObjectAccessor(store_path);

    accessor->set_path_display("«" + input.to_string(true) + "»");

    return {accessor, input};
  }

  bool isLocked(const settings_t& settings, const input_t& input) const override {
    return (bool)input.getRev();
  }

  std::optional<std::string> get_fingerprint(store_t& store, const input_t& input) const override {
    if (auto rev = input.getRev()) {
      return "hg:" + rev->git_rev();
    } else {
      return std::nullopt;
    }
  }
};

static auto r_mercurial_input_scheme =
    on_startup_t([] { register_input_scheme(std::make_unique<mercurial_input_scheme_t>()); });

} // namespace nix::fetchers
