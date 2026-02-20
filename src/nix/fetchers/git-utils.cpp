#include "nix/fetchers/git-utils.h"

#include <iostream>
#include <queue>
#include <ranges>
#include <regex>
#include <span>

#include <git2/attr.h>
#include <git2/blob.h>
#include <git2/branch.h>
#include <git2/commit.h>
#include <git2/config.h>
#include <git2/describe.h>
#include <git2/errors.h>
#include <git2/global.h>
#include <git2/indexer.h>
#include <git2/object.h>
#include <git2/odb.h>
#include <git2/odb_backend.h>
#include <git2/refs.h>
#include <git2/remote.h>
#include <git2/repository.h>
#include <git2/revparse.h>
#include <git2/status.h>
#include <git2/submodule.h>
#include <git2/sys/mempack.h>
#include <git2/sys/odb_backend.h>
#include <git2/sys/repository.h>
#include <git2/tag.h>
#include <git2/tree.h>

#include <boost/unordered/concurrent_flat_set.hpp>
#include <boost/unordered/unordered_flat_map.hpp>
#include <boost/unordered/unordered_flat_set.hpp>

#include "nix/fetchers/cache.h"
#include "nix/fetchers/fetch-settings.h"
#include "nix/fetchers/git-lfs-fetch.h"
#include "nix/util/base-n.h"
#include "nix/util/executable-path.h"
#include "nix/util/finally.h"
#include "nix/util/fs-sink.h"
#include "nix/util/pool.h"
#include "nix/util/processes.h"
#include "nix/util/signals.h"
#include "nix/util/sync.h"
#include "nix/util/thread-pool.h"
#include "nix/util/users.h"
#include "nix/util/util.h"

namespace std {

template <>
struct hash<git_oid> {
  size_t operator()(const git_oid& oid) const { return *(size_t*)oid.id; }
};

} // namespace std

std::ostream& operator<<(std::ostream& str, const git_oid& oid) {
  str << git_oid_tostr_s(&oid);
  return str;
}

bool operator==(const git_oid& oid1, const git_oid& oid2) {
  return git_oid_equal(&oid1, &oid2);
}

namespace nix {

struct git_source_accessor_t;

typedef std::unique_ptr<git_repository, Deleter<git_repository_free>> Repository;
typedef std::unique_ptr<git_tree_entry, Deleter<git_tree_entry_free>> tree_entry;
typedef std::unique_ptr<git_tree, Deleter<git_tree_free>> tree_t;
typedef std::unique_ptr<git_treebuilder, Deleter<git_treebuilder_free>> TreeBuilder;
typedef std::unique_ptr<git_blob, Deleter<git_blob_free>> blob;
typedef std::unique_ptr<git_object, Deleter<git_object_free>> Object;
typedef std::unique_ptr<git_commit, Deleter<git_commit_free>> Commit;
typedef std::unique_ptr<git_reference, Deleter<git_reference_free>> Reference;
typedef std::unique_ptr<git_describe_result, Deleter<git_describe_result_free>> DescribeResult;
typedef std::unique_ptr<git_status_list, Deleter<git_status_list_free>> StatusList;
typedef std::unique_ptr<git_remote, Deleter<git_remote_free>> Remote;
typedef std::unique_ptr<git_config, Deleter<git_config_free>> GitConfig;
typedef std::unique_ptr<git_config_iterator, Deleter<git_config_iterator_free>> ConfigIterator;
typedef std::unique_ptr<git_odb, Deleter<git_odb_free>> ObjectDb;
typedef std::unique_ptr<git_packbuilder, Deleter<git_packbuilder_free>> PackBuilder;
typedef std::unique_ptr<git_indexer, Deleter<git_indexer_free>> Indexer;

static Hash to_hash(const git_oid& oid) {
#ifdef GIT_EXPERIMENTAL_SHA256
  assert(oid.type == GIT_OID_SHA1);
#endif
  Hash hash(hash_algorithm_t::SHA1);
  memcpy(hash.hash(), oid.id, hash.hash_size());
  return hash;
}

static void init_lib_git2() {
  static std::once_flag initialized;
  std::call_once(initialized, []() {
    if (git_libgit2_init() < 0)
      throw Error("initialising libgit2: %s", git_error_last()->message);
  });
}

static git_oid hash_to_oid(const Hash& hash) {
  git_oid oid;
#ifdef GIT_EXPERIMENTAL_SHA256
  if (git_oid_fromstr(&oid, hash.git_rev().c_str(), GIT_OID_SHA1))
#else
  if (git_oid_fromstr(&oid, hash.git_rev().c_str()))
#endif
    throw Error("cannot convert '%s' to a Git OID", hash.git_rev());
  return oid;
}

static Object lookup_object(git_repository* repo, const git_oid& oid,
                            git_object_t type = GIT_OBJECT_ANY) {
  Object obj;
  if (git_object_lookup(Setter(obj), repo, &oid, type)) {
    auto err = git_error_last();
    throw Error("getting Git object '%s': %s", oid, err->message);
  }
  return obj;
}

template <typename T>
static T peel_object(git_object* obj, git_object_t type) {
  T obj2;
  if (git_object_peel((git_object**)(typename T::pointer*)Setter(obj2), obj, type)) {
    auto err = git_error_last();
    throw Error("peeling Git object '%s': %s", *git_object_id(obj), err->message);
  }
  return obj2;
}

template <typename T>
static T dup_object(typename T::pointer obj) {
  T obj2;
  if (git_object_dup((git_object**)(typename T::pointer*)Setter(obj2), (git_object*)obj))
    throw Error("duplicating object '%s': %s", *git_object_id((git_object*)obj),
                git_error_last()->message);
  return obj2;
}

/**
 * Peel the specified object (i.e. follow tag and commit objects) to
 * either a blob or a tree.
 */
static Object peel_to_tree_or_blob(git_object* obj) {
  /* git_object_peel() doesn't handle blob objects, so handle those
     specially. */
  if (git_object_type(obj) == GIT_OBJECT_BLOB)
    return dup_object<Object>(obj);
  else
    return peel_object<Object>(obj, GIT_OBJECT_TREE);
}

struct pack_builder_context_t {
  std::exception_ptr exception;

  void handle_exception(const char* activity, int err_code) {
    switch (err_code) {
      case GIT_OK:
        break;
      case GIT_EUSER:
        if (!exception)
          panic("PackBuilderContext::handleException: user error, but exception was not set");

        std::rethrow_exception(exception);
      default:
        throw Error("%s: %i, %s", uncolored_t(activity), err_code, git_error_last()->message);
    }
  }
};

extern "C" {

/**
 * A `git_packbuilder_progress` implementation that aborts the pack building if needed.
 */
static int pack_builder_progress_check_interrupt(int stage, uint32_t current, uint32_t total,
                                                 void* payload) {
  pack_builder_context_t& args = *(pack_builder_context_t*)payload;
  try {
    check_interrupt();
    return GIT_OK;
  } catch (const std::exception& e) {
    args.exception = std::current_exception();
    return GIT_EUSER;
  }
};

static git_packbuilder_progress PACKBUILDER_PROGRESS_CHECK_INTERRUPT =
    &pack_builder_progress_check_interrupt;

} // extern "C"

static void init_repo_atomically(std::filesystem::path& path, GitRepo::Options options) {
  if (path_exists(path.string()))
    return;

  if (!options.create)
    throw Error("Git repository %s does not exist.", path);

  std::filesystem::path tmp_dir = create_temp_dir(path.parent_path());
  auto_delete_t del_tmp_dir(tmp_dir, true);
  Repository tmp_repo;

  if (git_repository_init(Setter(tmp_repo), tmp_dir.string().c_str(), options.bare))
    throw Error("creating Git repository %s: %s", path, git_error_last()->message);
  try {
    std::filesystem::rename(tmp_dir, path);
  } catch (std::filesystem::filesystem_error& e) {
    // Someone may race us to create the repository.
    if (e.code() == std::errc::file_exists
        // `path` may be attempted to be deleted by s::f::rename, in which case the code is:
        || e.code() == std::errc::directory_not_empty) {
      return;
    } else
      throw sys_error_t("moving temporary git repository from %s to %s", tmp_dir, path);
  }
  // we successfully moved the repository, so the temporary directory no longer exists.
  del_tmp_dir.cancel();
}

struct git_repo_impl_t : GitRepo, std::enable_shared_from_this<git_repo_impl_t> {
  /** Location of the repository on disk. */
  std::filesystem::path path;

  Options options;

  /**
   * libgit2 repository. Note that new objects are not written to disk,
   * because we are using a mempack backend. For writing to disk, see
   * `flush()`, which is also called by `GitFileSystemObjectSink::sync()`.
   */
  Repository repo;

  /**
   * In-memory object store for efficient batched writing to packfiles.
   * Owned by `repo`.
   */
  git_odb_backend* mempack_backend = nullptr;

  /**
   * On-disk packfile object store.
   * Owned by `repo`.
   */
  git_odb_backend* pack_backend = nullptr;

  git_repo_impl_t(std::filesystem::path _path, Options _options)
      : path(std::move(_path)), options(_options) {
    init_lib_git2();

    init_repo_atomically(path, options);
    if (git_repository_open(Setter(repo), path.string().c_str()))
      throw Error("opening Git repository %s: %s", path, git_error_last()->message);

    ObjectDb odb;
    if (options.packfilesOnly) {
      /* Create a fresh object database because by default the repo also
         loose object backends. We are not using any of those for the
         tarball cache, but libgit2 still does a bunch of unnecessary
         syscalls that always fail with ENOENT. NOTE: We are only creating
         a libgit2 object here and not modifying the repo. Think of this as
         enabling the specific backend.
         */

#ifdef GIT_EXPERIMENTAL_SHA256
      if (git_odb_new(Setter(odb), nullptr))
#else
      if (git_odb_new(Setter(odb)))
#endif
        throw Error("creating Git object database: %s", git_error_last()->message);

#ifdef GIT_EXPERIMENTAL_SHA256
      if (git_odb_backend_pack(&pack_backend, (path / "objects").string().c_str(), nullptr))
#else
      if (git_odb_backend_pack(&pack_backend, (path / "objects").string().c_str()))
#endif
        throw Error("creating pack backend: %s", git_error_last()->message);

      if (git_odb_add_backend(odb.get(), pack_backend, 1))
        throw Error("adding pack backend to Git object database: %s", git_error_last()->message);
    } else {
      if (git_repository_odb(Setter(odb), repo.get()))
        throw Error("getting Git object database: %s", git_error_last()->message);
    }

    // mempack_backend will be owned by the repository, so we are not expected to free it ourselves.
    if (git_mempack_new(&mempack_backend))
      throw Error("creating mempack backend: %s", git_error_last()->message);

    if (git_odb_add_backend(odb.get(), mempack_backend, 999))
      throw Error("adding mempack backend to Git object database: %s", git_error_last()->message);

    if (options.packfilesOnly) {
      if (git_repository_set_odb(repo.get(), odb.get()))
        throw Error("setting Git object database: %s", git_error_last()->message);
    }
  }

  operator git_repository*() { return repo.get(); }

  void flush() override {
    check_interrupt();

    git_buf buf = GIT_BUF_INIT;
    finally_t _disposeBuf{[&] { git_buf_dispose(&buf); }};
    PackBuilder packBuilder;
    pack_builder_context_t packBuilderContext;
    git_packbuilder_new(Setter(packBuilder), *this);
    git_packbuilder_set_callbacks(packBuilder.get(), PACKBUILDER_PROGRESS_CHECK_INTERRUPT,
                                  &packBuilderContext);
    git_packbuilder_set_threads(packBuilder.get(), 0 /* autodetect */);

    packBuilderContext.handle_exception(
        "preparing packfile", git_mempack_write_thin_pack(mempack_backend, packBuilder.get()));
    check_interrupt();
    packBuilderContext.handle_exception("writing packfile",
                                        git_packbuilder_write_buf(&buf, packBuilder.get()));
    check_interrupt();

    std::string repo_path = std::string(git_repository_path(repo.get()));
    while (!repo_path.empty() && repo_path.back() == '/')
      repo_path.pop_back();
    std::string pack_dir_path = repo_path + "/objects/pack";

    // TODO (performance): could the indexing be done in a separate thread?
    //                     we'd need a more streaming variation of
    //                     git_packbuilder_write_buf, or incur the cost of
    //                     copying parts of the buffer to a separate thread.
    //                     (synchronously on the git_packbuilder_write_buf thread)
    Indexer indexer;
    git_indexer_progress stats;
#ifdef GIT_EXPERIMENTAL_SHA256
    if (git_indexer_new(Setter(indexer), pack_dir_path.c_str(), nullptr))
#else
    if (git_indexer_new(Setter(indexer), pack_dir_path.c_str(), 0, nullptr, nullptr))
#endif
      throw Error("creating git packfile indexer: %s", git_error_last()->message);

    // TODO: provide index callback for checkInterrupt() termination
    //       though this is about an order of magnitude faster than the packbuilder
    //       expect up to 1 sec latency due to uninterruptible git_indexer_append.
    constexpr size_t chunk_size = 128 * 1024;
    for (size_t offset = 0; offset < buf.size; offset += chunk_size) {
      if (git_indexer_append(indexer.get(), buf.ptr + offset,
                             std::min(chunk_size, buf.size - offset), &stats))
        throw Error("appending to git packfile index: %s", git_error_last()->message);
      check_interrupt();
    }

    if (git_indexer_commit(indexer.get(), &stats))
      throw Error("committing git packfile index: %s", git_error_last()->message);

    if (git_mempack_reset(mempack_backend))
      throw Error("resetting git mempack backend: %s", git_error_last()->message);

    check_interrupt();
  }

  /**
   * Return a connection pool for this repo. Useful for
   * multithreaded access.
   */
  pool_t<git_repo_impl_t> getPool() {
    // TODO: as an optimization, it would be nice to include `this` in the pool.
    return pool_t<git_repo_impl_t>(std::numeric_limits<size_t>::max(),
                                 [this]() -> ref<git_repo_impl_t> {
                                   auto repo = make_ref<git_repo_impl_t>(path, options);

                                   /* Monkey-patching the pack backend to only read the pack
                                      directory once. Otherwise it will do a readdir for each added
                                      oid when it's not found and that translates to ~6 syscalls.
                                      Since we are never writing pack files until flushing we can
                                      force the odb backend to read the directory just once. It's
                                      very convenient that the vtable is semi-public interface and
                                      is up for grabs.

                                      This is purely an optimization for our use-case with a tarball
                                      cache. libgit2 calls refresh() if the backend provides it when
                                      an oid isn't found. We are only writing objects to a mempack
                                      (it has higher priority) and there isn't a realistic use-case
                                      where a previously missing object would appear from thin air
                                      on the disk (unless another process happens to be unpacking a
                                      similar tarball to the cache at the same time, but that's a
                                      very unrealistic scenario).
                                   */
                                   if (auto* backend = repo->pack_backend)
                                     backend->refresh = nullptr;

                                   return repo;
                                 });
  }

  uint64_t get_rev_count(const Hash& rev) override {
    boost::concurrent_flat_set<git_oid, std::hash<git_oid>> done;

    auto startCommit =
        peel_object<Commit>(lookup_object(*this, hash_to_oid(rev)).get(), GIT_OBJECT_COMMIT);
    auto startOid = *git_commit_id(startCommit.get());
    done.insert(startOid);

    auto repo_pool(getPool());

    thread_pool_t pool;

    auto process = [&done, &pool, &repo_pool](this auto const& process,
                                              const git_oid& oid) -> void {
      auto repo(repo_pool.get());

      auto _commit = lookup_object(*repo, oid, GIT_OBJECT_COMMIT);
      auto commit = (const git_commit*)&*_commit;

      for (auto n : std::views::iota(0U, git_commit_parentcount(commit))) {
        auto parentOid = git_commit_parent_id(commit, n);
        if (!parentOid) {
          throw Error(
              "Failed to retrieve the parent of Git commit '%s': %s. "
              "This may be due to an incomplete repository history. "
              "To resolve this, either enable the shallow parameter in your flake URL (?shallow=1) "
              "or add set the shallow parameter to true in builtins.fetchGit, "
              "or fetch the complete history for this branch.",
              *git_commit_id(commit), git_error_last()->message);
        }
        if (done.insert(*parentOid))
          pool.enqueue(std::bind(process, *parentOid));
      }
    };

    pool.enqueue(std::bind(process, startOid));

    pool.process();

    return done.size();
  }

  uint64_t get_last_modified(const Hash& rev) override {
    auto commit =
        peel_object<Commit>(lookup_object(*this, hash_to_oid(rev)).get(), GIT_OBJECT_COMMIT);

    return git_commit_time(commit.get());
  }

  bool is_shallow() override { return git_repository_is_shallow(*this); }

  void setRemote(const std::string& name, const std::string& url) override {
    if (git_remote_set_url(*this, name.c_str(), url.c_str()))
      throw Error("setting remote '%s' URL to '%s': %s", name, url, git_error_last()->message);
  }

  Hash resolveRef(std::string ref) override {
    Object object;

    // Using the rev-parse notation which libgit2 supports, make sure we peel
    // the ref ultimately down to the underlying commit.
    // This is to handle the case where it may be an annotated tag which itself has
    // an object_id.
    std::string peeledRef = ref + "^{commit}";
    if (git_revparse_single(Setter(object), *this, peeledRef.c_str()))
      throw Error("resolving Git reference '%s': %s", ref, git_error_last()->message);
    auto oid = git_object_id(object.get());
    return to_hash(*oid);
  }

  std::vector<submodule_t> parseSubmodules(const std::filesystem::path& configFile) {
    GitConfig config;
    if (git_config_open_ondisk(Setter(config), configFile.string().c_str()))
      throw Error("parsing .gitmodules file: %s", git_error_last()->message);

    ConfigIterator it;
    if (git_config_iterator_glob_new(Setter(it), config.get(),
                                     "^submodule\\..*\\.(path|url|branch)$"))
      throw Error("iterating over .gitmodules: %s", git_error_last()->message);

    string_map_t entries;

    while (true) {
      git_config_entry* entry = nullptr;
      if (auto err = git_config_next(&entry, it.get())) {
        if (err == GIT_ITEROVER)
          break;
        throw Error("iterating over .gitmodules: %s", git_error_last()->message);
      }
      entries.emplace(entry->name + 10, entry->value);
    }

    std::vector<submodule_t> result;

    for (auto& [key, value] : entries) {
      if (!has_suffix(key, ".path"))
        continue;
      std::string key2(key, 0, key.size() - 5);
      auto path = canon_path_t(value);
      result.push_back(submodule_t{
          .path = path,
          .url = entries[key2 + ".url"],
          .branch = entries[key2 + ".branch"],
      });
    }

    return result;
  }

  // Helper for statusCallback below.
  static int statusCallbackTrampoline(const char* path, unsigned int statusFlags, void* payload) {
    return (*((std::function<int(const char* path, unsigned int statusFlags)>*)payload))(
        path, statusFlags);
  }

  WorkdirInfo getWorkdirInfo() override {
    WorkdirInfo info;

    /* Get the head revision, if any. */
    git_oid headRev;
    if (auto err = git_reference_name_to_id(&headRev, *this, "HEAD")) {
      if (err != GIT_ENOTFOUND)
        throw Error("resolving HEAD: %s", git_error_last()->message);
    } else
      info.headRev = to_hash(headRev);

    /* Get all tracked files and determine whether the working
       directory is dirty. */
    std::function<int(const char* path, unsigned int statusFlags)> statusCallback =
        [&](const char* path, unsigned int statusFlags) {
          if (!(statusFlags & GIT_STATUS_INDEX_DELETED) && !(statusFlags & GIT_STATUS_WT_DELETED)) {
            info.files.insert(canon_path_t(path));
            if (statusFlags != GIT_STATUS_CURRENT)
              info.dirtyFiles.insert(canon_path_t(path));
          } else
            info.deletedFiles.insert(canon_path_t(path));
          if (statusFlags != GIT_STATUS_CURRENT)
            info.isDirty = true;
          return 0;
        };

    git_status_options options = GIT_STATUS_OPTIONS_INIT;
    options.flags |= GIT_STATUS_OPT_INCLUDE_UNMODIFIED;
    options.flags |= GIT_STATUS_OPT_EXCLUDE_SUBMODULES;
    if (git_status_foreach_ext(*this, &options, &statusCallbackTrampoline, &statusCallback))
      throw Error("getting working directory status: %s", git_error_last()->message);

    /* Get submodule info. */
    auto modulesFile = path / ".gitmodules";
    if (path_exists(modulesFile.string()))
      info.submodules = parseSubmodules(modulesFile);

    return info;
  }

  std::optional<std::string> getWorkdirRef() override {
    Reference ref;
    if (git_reference_lookup(Setter(ref), *this, "HEAD"))
      throw Error("looking up HEAD: %s", git_error_last()->message);

    if (auto target = git_reference_symbolic_target(ref.get()))
      return target;

    return std::nullopt;
  }

  std::vector<std::tuple<submodule_t, Hash>> getSubmodules(const Hash& rev,
                                                           bool export_ignore) override;

  std::string resolveSubmoduleUrl(const std::string& url) override {
    git_buf buf = GIT_BUF_INIT;
    if (git_submodule_resolve_url(&buf, *this, url.c_str()))
      throw Error("resolving Git submodule URL '%s'", url);
    finally_t cleanup = [&]() { git_buf_dispose(&buf); };

    std::string res(buf.ptr);
    return res;
  }

  bool hasObject(const Hash& oid_) override {
    auto oid = hash_to_oid(oid_);

    Object obj;
    if (auto err_code = git_object_lookup(Setter(obj), *this, &oid, GIT_OBJECT_ANY)) {
      if (err_code == GIT_ENOTFOUND)
        return false;
      auto err = git_error_last();
      throw Error("getting Git object '%s': %s", oid, err->message);
    }

    return true;
  }

  /**
   * A 'GitSourceAccessor' with no regard for export-ignore.
   */
  ref<git_source_accessor_t> get_raw_accessor(const Hash& rev, const GitAccessorOptions& options);

  ref<source_accessor_t> get_accessor(const Hash& rev, const GitAccessorOptions& options,
                                   std::string display_prefix) override;

  ref<source_accessor_t> get_accessor(const WorkdirInfo& wd, const GitAccessorOptions& options,
                                   MakeNotAllowedError e) override;

  ref<GitFileSystemObjectSink> get_file_system_object_sink() override;

  void fetch(const std::string& url, const std::string& refspec, bool shallow) override {
    activity_t act(*logger, lvl_talkative, act_fetch_tree,
                   fmt("fetching Git repository '%s'", url));

    // TODO: implement git-credential helper support (preferably via libgit2, which as of 2024-01
    // does not support that)
    //       then use code that was removed in this commit (see blame)

    if (executable_path_t::load().find_name("git")) {
      auto dir = this->path;
      strings_t git_args{"-C", dir.string(), "--git-dir", ".", "fetch", "--progress", "--force"};
      if (shallow)
        append(git_args, {"--depth", "1"});
      append(git_args, {std::string("--"), url, refspec});

      auto status =
          run_program(run_options_t{.program = "git", .args = git_args, .is_interactive = true})
              .first;

      if (status > 0)
        throw Error("Failed to fetch git repository '%s'", url);
    } else {
      // Fall back to using libgit2 for fetching. This does not
      // support SSH very well.
      Remote remote;

      if (git_remote_create_anonymous(Setter(remote), *this, url.c_str()))
        throw Error("cannot create Git remote '%s': %s", url, git_error_last()->message);

      char* refspecs[] = {(char*)refspec.c_str()};
      git_strarray refspecs2{.strings = refspecs, .count = 1};

      git_fetch_options opts = GIT_FETCH_OPTIONS_INIT;
      // FIXME: for some reason, shallow fetching over ssh barfs
      // with "could not read from remote repository".
      opts.depth = shallow && parse_url(url).scheme() != "ssh" ? 1 : GIT_FETCH_DEPTH_FULL;
      opts.callbacks.payload = &act;

      if (git_remote_fetch(remote.get(), &refspecs2, &opts, nullptr))
        throw Error("fetching '%s' from '%s': %s", refspec, url, git_error_last()->message);
    }
  }

  void verify_commit(const Hash& rev,
                     const std::vector<fetchers::public_key_t>& public_keys) override {
    // Map of SSH key types to their internal OpenSSH representations
    static const boost::unordered_flat_map<std::string_view, std::string_view> key_type_map = {
        {"ssh-dsa", "ssh-dsa"},
        {"ssh-ecdsa", "ssh-ecdsa"},
        {"ssh-ecdsa-sk", "sk-ecdsa-sha2-nistp256@openssh.com"},
        {"ssh-ed25519", "ssh-ed25519"},
        {"ssh-ed25519-sk", "sk-ssh-ed25519@openssh.com"},
        {"ssh-rsa", "ssh-rsa"}};

    // Create ad-hoc allowedSignersFile and populate it with publicKeys
    auto allowed_signers_file = create_temp_file().second;
    std::string allowed_signers;

    for (const fetchers::public_key_t& k : public_keys) {
      auto it = key_type_map.find(k.type);
      if (it == key_type_map.end()) {
        std::string supportedTypes;
        for (const auto& [type, _] : key_type_map) {
          supportedTypes += fmt("  %s\n", type);
        }
        throw Error("Invalid SSH key type '%s' in publicKeys.\n"
                    "Please use one of:\n%s",
                    k.type, supportedTypes);
      }

      allowed_signers += fmt("* %s %s\n", it->second, k.key);
    }
    write_file(allowed_signers_file, allowed_signers);

    // Run verification command
    auto [status, output] = run_program(run_options_t{
        .program = "git",
        .args = {"-c", "gpg.ssh.allowedSignersFile=" + allowed_signers_file, "-C", path.string(),
                 "verify-commit", rev.git_rev()},
        .merge_stderr_to_stdout = true,
    });

    /* Evaluate result through status code and checking if public
       key fingerprints appear on stderr. This is necessary
       because the git command might also succeed due to the
       commit being signed by gpg keys that are present in the
       users key agent. */
    std::string re = R"(Good "git" signature for \* with .* key SHA256:[)";
    for (const fetchers::public_key_t& k : public_keys) {
      // Calculate sha256 fingerprint from public key and escape the regex symbol '+' to match the
      // key literally
      std::string keyDecoded;
      try {
        keyDecoded = base64::decode(k.key);
      } catch (Error& e) {
        e.add_trace({}, "while decoding public key '%s' used for git signature", k.key);
        throw;
      }
      auto fingerprint = trim(hash_string(hash_algorithm_t::SHA256, keyDecoded)
                                  .to_string(nix::hash_format_t::base64, false),
                              "=");
      auto escaped_fingerprint = std::regex_replace(fingerprint, std::regex("\\+"), "\\+");
      re += "(" + escaped_fingerprint + ")";
    }
    re += "]";
    if (status == 0 && std::regex_search(output, std::regex(re)))
      printTalkative("Signature verification on commit %s succeeded.", rev.git_rev());
    else
      throw Error("Commit signature verification on commit %s failed: %s", rev.git_rev(), output);
  }

  Hash treeHashToNarHash(const fetchers::settings_t& settings, const Hash& tree_hash) override {
    auto accessor = get_accessor(tree_hash, {}, "");

    fetchers::cache_t::Key cache_key{"treeHashToNarHash", {{"treeHash", tree_hash.git_rev()}}};

    if (auto res = settings.get_cache()->lookup(cache_key))
      return Hash::parse_any(fetchers::get_str_attr(*res, "narHash"), hash_algorithm_t::SHA256);

    auto nar_hash = accessor->hash_path(canon_path_t::root);

    settings.get_cache()->upsert(
        cache_key, fetchers::Attrs({{"narHash", nar_hash.to_string(hash_format_t::sri, true)}}));

    return nar_hash;
  }

  Hash dereferenceSingletonDirectory(const Hash& oid_) override {
    auto oid = hash_to_oid(oid_);

    auto _tree = lookup_object(*this, oid, GIT_OBJECT_TREE);
    auto tree = (const git_tree*)&*_tree;

    if (git_tree_entrycount(tree) == 1) {
      auto entry = git_tree_entry_byindex(tree, 0);
      auto mode = git_tree_entry_filemode(entry);
      if (mode == GIT_FILEMODE_TREE)
        oid = *git_tree_entry_id(entry);
    }

    return to_hash(oid);
  }
};

ref<GitRepo> GitRepo::openRepo(const std::filesystem::path& path, GitRepo::Options options) {
  return make_ref<git_repo_impl_t>(path, options);
}

std::string GitAccessorOptions::makeFingerprint(const Hash& rev) const {
  return "git:" + rev.git_rev() + (export_ignore ? ";e" : "") + (smudgeLfs ? ";l" : "");
}

/**
 * raw_t git tree input accessor.
 */
struct git_source_accessor_t : source_accessor_t {
  struct State {
    ref<git_repo_impl_t> repo;
    Object root;
    std::optional<lfs::Fetch> lfs_fetch = std::nullopt;
    GitAccessorOptions options;
  };

  sync_t<State> state_;

  git_source_accessor_t(ref<git_repo_impl_t> repo_, const Hash& rev,
                        const GitAccessorOptions& options)
      : state_{State{
            .repo = repo_,
            .root = peel_to_tree_or_blob(lookup_object(*repo_, hash_to_oid(rev)).get()),
            .lfs_fetch = options.smudgeLfs
                             ? std::make_optional(lfs::Fetch(*repo_, hash_to_oid(rev)))
                             : std::nullopt,
            .options = options,
        }} {
    fingerprint = options.makeFingerprint(rev);
  }

  std::string readBlob(const canon_path_t& path, bool symlink) {
    auto state(state_.lock());

    const auto blob = get_blob(*state, path, symlink);

    if (state->lfs_fetch) {
      if (state->lfs_fetch->shouldFetch(path)) {
        string_sink_t s;
        try {
          // FIXME: do we need to hold the state lock while
          // doing this?
          auto contents = std::string((const char*)git_blob_rawcontent(blob.get()),
                                      git_blob_rawsize(blob.get()));
          state->lfs_fetch->fetch(contents, path, s,
                                  [&s](uint64_t size) { s.str().reserve(size); });
        } catch (Error& e) {
          e.add_trace({}, "while smudging git-lfs file '%s'", path);
          throw;
        }
        return s.str();
      }
    }

    return std::string((const char*)git_blob_rawcontent(blob.get()), git_blob_rawsize(blob.get()));
  }

  std::string read_file(const canon_path_t& path) override { return readBlob(path, false); }

  bool path_exists(const canon_path_t& path) override {
    auto state(state_.lock());
    return path.is_root() ? true : (bool)lookup(*state, path);
  }

  std::optional<stat_t> maybe_lstat(const canon_path_t& path) override {
    auto state(state_.lock());

    if (path.is_root())
      return stat_t{.type = git_object_type(state->root.get()) == GIT_OBJECT_TREE ? t_directory
                                                                                  : t_regular};

    auto entry = lookup(*state, path);
    if (!entry)
      return std::nullopt;

    auto mode = git_tree_entry_filemode(entry);

    if (mode == GIT_FILEMODE_TREE)
      return stat_t{.type = t_directory};

    else if (mode == GIT_FILEMODE_BLOB)
      return stat_t{.type = t_regular};

    else if (mode == GIT_FILEMODE_BLOB_EXECUTABLE)
      return stat_t{.type = t_regular, .is_executable = true};

    else if (mode == GIT_FILEMODE_LINK)
      return stat_t{.type = t_symlink};

    else if (mode == GIT_FILEMODE_COMMIT)
      // Treat submodules as an empty directory.
      return stat_t{.type = t_directory};

    else
      throw Error("file '%s' has an unsupported Git file type");
  }

  dir_entries_t read_directory(const canon_path_t& path) override {
    auto state(state_.lock());

    return std::visit(overloaded{[&](tree_t tree) {
                                   dir_entries_t res;

                                   auto count = git_tree_entrycount(tree.get());

                                   for (size_t n = 0; n < count; ++n) {
                                     auto entry = git_tree_entry_byindex(tree.get(), n);
                                     // FIXME: add to cache
                                     res.emplace(std::string(git_tree_entry_name(entry)),
                                                 dir_entry_t{});
                                   }

                                   return res;
                                 },
                                 [&](submodule_t) { return dir_entries_t(); }},
                      get_tree(*state, path));
  }

  std::string read_link(const canon_path_t& path) override { return readBlob(path, true); }

  /**
   * If `path` exists and is a submodule, return its
   * revision. Otherwise return nothing.
   */
  std::optional<Hash> getSubmoduleRev(const canon_path_t& path) {
    auto state(state_.lock());

    auto entry = lookup(*state, path);

    if (!entry || git_tree_entry_type(entry) != GIT_OBJECT_COMMIT)
      return std::nullopt;

    return to_hash(*git_tree_entry_id(entry));
  }

  boost::unordered_flat_map<canon_path_t, tree_entry> lookupCache;

  /* Recursively look up 'path' relative to the root. */
  git_tree_entry* lookup(State& state, const canon_path_t& path) {
    auto i = lookupCache.find(path);
    if (i != lookupCache.end())
      return i->second.get();

    auto parent = path.parent();
    if (!parent)
      return nullptr;

    auto name = path.base_name().value();

    auto parent_tree = lookup_tree(state, *parent);
    if (!parent_tree)
      return nullptr;

    auto count = git_tree_entrycount(parent_tree->get());

    git_tree_entry* res = nullptr;

    /* Add all the tree entries to the cache to speed up
       subsequent lookups. */
    for (size_t n = 0; n < count; ++n) {
      auto entry = git_tree_entry_byindex(parent_tree->get(), n);

      tree_entry copy;
      if (git_tree_entry_dup(Setter(copy), entry))
        throw Error("dupping tree entry: %s", git_error_last()->message);

      auto entry_name = std::string_view(git_tree_entry_name(entry));

      if (entry_name == name)
        res = copy.get();

      auto path2 = *parent;
      path2.push(entry_name);
      lookupCache.emplace(path2, std::move(copy)).first->second.get();
    }

    return res;
  }

  std::optional<tree_t> lookup_tree(State& state, const canon_path_t& path) {
    if (path.is_root()) {
      if (git_object_type(state.root.get()) == GIT_OBJECT_TREE)
        return dup_object<tree_t>((git_tree*)&*state.root);
      else
        return std::nullopt;
    }

    auto entry = lookup(state, path);
    if (!entry || git_tree_entry_type(entry) != GIT_OBJECT_TREE)
      return std::nullopt;

    tree_t tree;
    if (git_tree_entry_to_object((git_object**)(git_tree**)Setter(tree), *state.repo, entry))
      throw Error("looking up directory '%s': %s", show_path(path), git_error_last()->message);

    return tree;
  }

  git_tree_entry* need(State& state, const canon_path_t& path) {
    auto entry = lookup(state, path);
    if (!entry)
      throw Error("'%s' does not exist", show_path(path));
    return entry;
  }

  struct submodule_t {};

  std::variant<tree_t, submodule_t> get_tree(State& state, const canon_path_t& path) {
    if (path.is_root()) {
      if (git_object_type(state.root.get()) == GIT_OBJECT_TREE)
        return dup_object<tree_t>((git_tree*)&*state.root);
      else
        throw Error("Git root object '%s' is not a directory", *git_object_id(state.root.get()));
    }

    auto entry = need(state, path);

    if (git_tree_entry_type(entry) == GIT_OBJECT_COMMIT)
      return submodule_t();

    if (git_tree_entry_type(entry) != GIT_OBJECT_TREE)
      throw Error("'%s' is not a directory", show_path(path));

    tree_t tree;
    if (git_tree_entry_to_object((git_object**)(git_tree**)Setter(tree), *state.repo, entry))
      throw Error("looking up directory '%s': %s", show_path(path), git_error_last()->message);

    return tree;
  }

  blob get_blob(State& state, const canon_path_t& path, bool expect_symlink) {
    if (!expect_symlink && git_object_type(state.root.get()) == GIT_OBJECT_BLOB)
      return dup_object<blob>((git_blob*)&*state.root);

    auto not_expected = [&]() {
      throw Error(expect_symlink ? "'%s' is not a symlink" : "'%s' is not a regular file",
                  show_path(path));
    };

    if (path.is_root())
      not_expected();

    auto entry = need(state, path);

    if (git_tree_entry_type(entry) != GIT_OBJECT_BLOB)
      not_expected();

    auto mode = git_tree_entry_filemode(entry);
    if (expect_symlink) {
      if (mode != GIT_FILEMODE_LINK)
        not_expected();
    } else {
      if (mode != GIT_FILEMODE_BLOB && mode != GIT_FILEMODE_BLOB_EXECUTABLE)
        not_expected();
    }

    blob blob;
    if (git_tree_entry_to_object((git_object**)(git_blob**)Setter(blob), *state.repo, entry))
      throw Error("looking up file '%s': %s", show_path(path), git_error_last()->message);

    return blob;
  }
};

struct git_export_ignore_source_accessor_t : CachingFilteringSourceAccessor {
  ref<git_repo_impl_t> repo;
  std::optional<Hash> rev;

  git_export_ignore_source_accessor_t(ref<git_repo_impl_t> repo, ref<source_accessor_t> next,
                                      std::optional<Hash> rev)
      : CachingFilteringSourceAccessor(
            next,
            [&](const canon_path_t& path) {
              return RestrictedPathError(fmt(
                  "'%s' does not exist because it was fetched with exportIgnore enabled", path));
            }),
        repo(repo),
        rev(rev) {}

  bool gitAttrGet(const canon_path_t& path, const char* attr_name, const char*& valueOut) {
    const char* pathCStr = path.rel_c_str();

    if (rev) {
      git_attr_options opts = GIT_ATTR_OPTIONS_INIT;
      opts.attr_commit_id = hash_to_oid(*rev);
      // TODO: test that gitattributes from global and system are not used
      //       (ie more or less: home and etc - both of them!)
      opts.flags = GIT_ATTR_CHECK_INCLUDE_COMMIT | GIT_ATTR_CHECK_NO_SYSTEM;
      return git_attr_get_ext(&valueOut, *repo, &opts, pathCStr, attr_name);
    } else {
      return git_attr_get(&valueOut, *repo, GIT_ATTR_CHECK_INDEX_ONLY | GIT_ATTR_CHECK_NO_SYSTEM,
                          pathCStr, attr_name);
    }
  }

  bool isExportIgnored(const canon_path_t& path) {
    const char* exportIgnoreEntry = nullptr;

    // GIT_ATTR_CHECK_INDEX_ONLY:
    // > It will use index only for creating archives or for a bare repo
    // > (if an index has been specified for the bare repo).
    // -- https://github.com/libgit2/libgit2/blob/HEAD/include/git2/attr.h#L113C62-L115C48
    if (gitAttrGet(path, "export-ignore", exportIgnoreEntry)) {
      if (git_error_last()->klass == GIT_ENOTFOUND)
        return false;
      else
        throw Error("looking up '%s': %s", show_path(path), git_error_last()->message);
    } else {
      // Official git will silently reject export-ignore lines that have
      // values. We do the same.
      return GIT_ATTR_IS_TRUE(exportIgnoreEntry);
    }
  }

  bool isAllowedUncached(const canon_path_t& path) override { return !isExportIgnored(path); }
};

struct git_file_system_object_sink_impl_t : GitFileSystemObjectSink {
  ref<git_repo_impl_t> repo;

  pool_t<git_repo_impl_t> repo_pool;

  unsigned int concurrency = std::min(std::thread::hardware_concurrency(), 10U);

  thread_pool_t workers{concurrency};

  /** Total file contents in flight. */
  std::atomic<size_t> total_buf_size{0};

  static constexpr std::size_t max_buf_size = 16 * 1024 * 1024;

  git_file_system_object_sink_impl_t(ref<git_repo_impl_t> repo)
      : repo(repo), repo_pool(repo->getPool()) {}

  ~git_file_system_object_sink_impl_t() {
    // Make sure the worker threads are destroyed before any state
    // they're referring to.
    workers.shutdown();
  }

  struct Child;

  /// A directory to be written as a Git tree.
  struct directory_t {
    std::map<std::string, Child> children;
    std::optional<git_oid> oid;

    Child& lookup(const canon_path_t& path) {
      assert(!path.is_root());
      auto parent = path.parent();
      auto cur = this;
      for (auto& name : *parent) {
        auto i = cur->children.find(std::string(name));
        if (i == cur->children.end())
          throw Error("path '%s' does not exist", path);
        auto dir = std::get_if<directory_t>(&i->second.file);
        if (!dir)
          throw Error("path '%s' has a non-directory parent", path);
        cur = dir;
      }

      auto i = cur->children.find(std::string(*path.base_name()));
      if (i == cur->children.end())
        throw Error("path '%s' does not exist", path);
      return i->second;
    }
  };

  size_t next_id = 0; // for Child.id

  struct Child {
    git_filemode_t mode;
    std::variant<directory_t, git_oid> file;

    /// Sequential numbering of the file in the tarball. This is
    /// used to make sure we only import the latest version of a
    /// path.
    size_t id{0};
  };

  struct State {
    directory_t root;
  };

  sync_t<State> _state;

  void add_node(State& state, const canon_path_t& path, Child&& child) {
    assert(!path.is_root());
    auto parent = path.parent();

    directory_t* cur = &state.root;

    for (auto& i : *parent) {
      auto child = std::get_if<directory_t>(
          &cur->children.emplace(std::string(i), Child{GIT_FILEMODE_TREE, {directory_t()}})
               .first->second.file);
      assert(child);
      cur = child;
    }

    std::string name(*path.base_name());

    if (auto prev = cur->children.find(name);
        prev == cur->children.end() || prev->second.id < child.id)
      cur->children.insert_or_assign(name, std::move(child));
  }

  void create_regular_file(const canon_path_t& path,
                           std::function<void(create_regular_file_sink_t&)> func) override {
    check_interrupt();

    /* Multithreaded blob writing. We read the incoming file data into memory and asynchronously
       write it to a git blob object. However, to avoid unbounded memory usage, if the amount of
       data in flight exceeds a threshold, we switch to writing directly to a git write stream. */

    using WriteStream = std::unique_ptr<::git_writestream, decltype([](::git_writestream* stream) {
                                          if (stream)
                                            stream->free(stream);
                                        })>;

    struct CRF : create_regular_file_sink_t {
      canon_path_t path;
      git_file_system_object_sink_impl_t& parent;
      WriteStream stream;
      std::optional<decltype(parent.repo_pool)::Handle> repo;

      std::string contents;
      bool executable = false;

      CRF(canon_path_t path, git_file_system_object_sink_impl_t& parent)
          : path(std::move(path)), parent(parent) {}

      ~CRF() { parent.total_buf_size -= contents.size(); }

      void operator()(std::string_view data) override {
        if (!stream) {
          contents.append(data);
          parent.total_buf_size += data.size();

          if (parent.total_buf_size > parent.max_buf_size) {
            repo.emplace(parent.repo_pool.get());

            if (git_blob_create_from_stream(Setter(stream), **repo, nullptr))
              throw Error("creating a blob stream object: %s", git_error_last()->message);

            if (stream->write(stream.get(), contents.data(), contents.size()))
              throw Error("writing a blob for tarball member '%s': %s", path,
                          git_error_last()->message);

            parent.total_buf_size -= contents.size();
            contents.clear();
          }
        } else {
          if (stream->write(stream.get(), data.data(), data.size()))
            throw Error("writing a blob for tarball member '%s': %s", path,
                        git_error_last()->message);
        }
      }

      void is_executable() override { executable = true; }
    };

    auto crf = std::make_shared<CRF>(path, *this);

    func(*crf);

    auto id = next_id++;

    if (crf->stream) {
      /* Finish the slow path by creating the blob object synchronously.
         Call .release(), since git_blob_create_from_stream_commit
         acquires ownership and frees the stream. */
      git_oid oid;
      if (git_blob_create_from_stream_commit(&oid, crf->stream.release()))
        throw Error("creating a blob object for '%s': %s", path, git_error_last()->message);
      add_node(*_state.lock(), crf->path,
               Child{crf->executable ? GIT_FILEMODE_BLOB_EXECUTABLE : GIT_FILEMODE_BLOB, oid, id});
      return;
    }

    /* Fast path: create the blob object in a separate thread. */
    workers.enqueue([this, crf{std::move(crf)}, id]() {
      auto repo(repo_pool.get());

      git_oid oid;
      if (git_blob_create_from_buffer(&oid, *repo, crf->contents.data(), crf->contents.size()))
        throw Error("creating a blob object for '%s' from in-memory buffer: %s", crf->path,
                    git_error_last()->message);

      add_node(*_state.lock(), crf->path,
               Child{crf->executable ? GIT_FILEMODE_BLOB_EXECUTABLE : GIT_FILEMODE_BLOB, oid, id});
    });
  }

  void create_directory(const canon_path_t& path) override {
    if (path.is_root())
      return;
    auto state(_state.lock());
    add_node(*state, path, {GIT_FILEMODE_TREE, directory_t()});
  }

  void create_symlink(const canon_path_t& path, const std::string& target) override {
    workers.enqueue([this, path, target]() {
      auto repo(repo_pool.get());

      git_oid oid;
      if (git_blob_create_from_buffer(&oid, *repo, target.c_str(), target.size()))
        throw Error("creating a blob object for tarball symlink member '%s': %s", path,
                    git_error_last()->message);

      auto state(_state.lock());
      add_node(*state, path, Child{GIT_FILEMODE_LINK, oid});
    });
  }

  std::map<canon_path_t, canon_path_t> hard_links;

  void create_hardlink(const canon_path_t& path, const canon_path_t& target) override {
    hard_links.insert_or_assign(path, target);
  }

  Hash flush() override {
    workers.process();

    /* Create hard links. */
    {
      auto state(_state.lock());
      for (auto& [path, target] : hard_links) {
        if (target.is_root())
          continue;
        try {
          auto child = state->root.lookup(target);
          auto oid = std::get_if<git_oid>(&child.file);
          if (!oid)
            throw Error("cannot create a hard link to a directory");
          add_node(*state, path, {child.mode, *oid});
        } catch (Error& e) {
          e.add_trace(nullptr, "while creating a hard link from '%s' to '%s'", path, target);
          throw;
        }
      }
    }

    // Flush all repo objects to disk.
    {
      auto repos = repo_pool.clear();
      thread_pool_t workers{repos.size()};
      for (auto& repo : repos)
        workers.enqueue([repo]() { repo->flush(); });
      workers.process();
    }

    // Write the Git trees to disk. Would be nice to have this multithreaded too, but that's hard
    // because a tree can't refer to an object that hasn't been written yet. Also it doesn't make a
    // big difference for performance.
    auto repo(repo_pool.get());

    [&](this const auto& visit, directory_t& node) -> void {
      check_interrupt();

      // Write the child directories.
      for (auto& child : node.children)
        if (auto dir = std::get_if<directory_t>(&child.second.file))
          visit(*dir);

      // Write this directory.
      git_treebuilder* b;
      if (git_treebuilder_new(&b, *repo, nullptr))
        throw Error("creating a tree builder: %s", git_error_last()->message);
      TreeBuilder builder(b);

      for (auto& [name, child] : node.children) {
        auto oid_p = std::get_if<git_oid>(&child.file);
        auto oid = oid_p ? *oid_p : std::get<directory_t>(child.file).oid.value();
        if (git_treebuilder_insert(nullptr, builder.get(), name.c_str(), &oid, child.mode))
          throw Error("adding a file to a tree builder: %s", git_error_last()->message);
      }

      git_oid oid;
      if (git_treebuilder_write(&oid, builder.get()))
        throw Error("creating a tree object: %s", git_error_last()->message);
      node.oid = oid;
    }(_state.lock()->root);

    repo->flush();

    return to_hash(_state.lock()->root.oid.value());
  }
};

ref<git_source_accessor_t> git_repo_impl_t::get_raw_accessor(const Hash& rev,
                                                             const GitAccessorOptions& options) {
  auto self = ref<git_repo_impl_t>(shared_from_this());
  return make_ref<git_source_accessor_t>(self, rev, options);
}

ref<source_accessor_t> git_repo_impl_t::get_accessor(const Hash& rev,
                                                  const GitAccessorOptions& options,
                                                  std::string display_prefix) {
  auto self = ref<git_repo_impl_t>(shared_from_this());
  ref<git_source_accessor_t> raw_git_accessor = get_raw_accessor(rev, options);
  raw_git_accessor->set_path_display(std::move(display_prefix));
  if (options.export_ignore)
    return make_ref<git_export_ignore_source_accessor_t>(self, raw_git_accessor, rev);
  else
    return raw_git_accessor;
}

ref<source_accessor_t> git_repo_impl_t::get_accessor(const WorkdirInfo& wd,
                                                  const GitAccessorOptions& options,
                                                  MakeNotAllowedError make_not_allowed_error) {
  auto self = ref<git_repo_impl_t>(shared_from_this());
  ref<source_accessor_t> file_accessor =
      AllowListSourceAccessor::create(make_fs_source_accessor(path),
                                      std::set<canon_path_t>{wd.files},
                                      // Always allow access to the root, but not its children.
                                      boost::unordered_flat_set<canon_path_t>{canon_path_t::root},
                                      std::move(make_not_allowed_error))
          .cast<source_accessor_t>();
  if (options.export_ignore)
    file_accessor =
        make_ref<git_export_ignore_source_accessor_t>(self, file_accessor, std::nullopt);
  return file_accessor;
}

ref<GitFileSystemObjectSink> git_repo_impl_t::get_file_system_object_sink() {
  return make_ref<git_file_system_object_sink_impl_t>(ref<git_repo_impl_t>(shared_from_this()));
}

std::vector<std::tuple<git_repo_impl_t::submodule_t, Hash>>
git_repo_impl_t::getSubmodules(const Hash& rev, bool export_ignore) {
  /* Read the .gitmodules files from this revision. */
  canon_path_t modulesFile(".gitmodules");

  auto accessor = get_accessor(rev, {.export_ignore = export_ignore}, "");
  if (!accessor->path_exists(modulesFile))
    return {};

  /* Parse it and get the revision of each submodule. */
  auto configS = accessor->read_file(modulesFile);

  auto [fdTemp, pathTemp] = create_temp_file("nix-git-submodules");
  try {
    write_full(fdTemp.get(), configS);
  } catch (sys_error_t& e) {
    e.add_trace({}, "while writing .gitmodules file to temporary file");
    throw;
  }

  std::vector<std::tuple<submodule_t, Hash>> result;

  auto rawAccessor = get_raw_accessor(rev, {});

  for (auto& submodule : parseSubmodules(pathTemp)) {
    /* Filter out .gitmodules entries that don't exist or are not
       submodules. */
    if (auto rev = rawAccessor->getSubmoduleRev(submodule.path))
      result.push_back({std::move(submodule), *rev});
  }

  return result;
}

namespace fetchers {

ref<GitRepo> settings_t::getTarballCache() const {
  /* v1: Had either only loose objects or thin packfiles referring to loose objects
   * v2: Must have only packfiles with no loose objects. Should get repacked periodically
   * for optimal packfiles.
   */
  static auto repo_dir = std::filesystem::path(get_cache_dir()) / "tarball-cache-v2";
  auto tarball_cache(_tarballCache.lock());
  if (!*tarball_cache)
    *tarball_cache =
        GitRepo::openRepo(repo_dir, {.create = true, .bare = true, .packfilesOnly = true});
  return ref<GitRepo>(*tarball_cache);
}

} // namespace fetchers

GitRepo::WorkdirInfo GitRepo::getCachedWorkdirInfo(const std::filesystem::path& path) {
  static sync_t<std::map<std::filesystem::path, WorkdirInfo>> _cache;
  {
    auto cache(_cache.lock());
    auto i = cache->find(path);
    if (i != cache->end())
      return i->second;
  }
  auto workdir_info = GitRepo::openRepo(path, {})->getWorkdirInfo();
  _cache.lock()->emplace(path, workdir_info);
  return workdir_info;
}

bool is_legal_ref_name(const std::string& ref_name) {
  init_lib_git2();

  /* Check for cases that don't get rejected by libgit2.
   * FIXME: libgit2 should reject this. */
  if (ref_name == "@")
    return false;

  /* libgit2 doesn't barf on DEL symbol.
   * FIXME: libgit2 should reject this. */
  if (ref_name.find('\177') != ref_name.npos)
    return false;

  for (auto* func : {
           git_reference_name_is_valid,
           git_branch_name_is_valid,
           git_tag_name_is_valid,
       }) {
    int valid = 0;
    if (func(&valid, ref_name.c_str()))
      throw Error("checking git reference '%s': %s", ref_name, git_error_last()->message);
    if (valid)
      return true;
  }

  return false;
}

} // namespace nix
