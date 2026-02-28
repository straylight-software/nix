#pragma once

#include "nix/fetchers/filtering-source-accessor.h"
#include "nix/util/fs-sink.h"

namespace nix {

namespace fetchers {
struct public_key_t;
struct settings_t;
} // namespace fetchers

/**
 * A sink that writes into a git repository. Note that nothing may be written
 * until `flush()` is called.
 */
struct GitFileSystemObjectSink : extended_file_system_object_sink_t {
  /**
   * Flush builder and return a final git hash.
   */
  virtual Hash flush() = 0;
};

struct GitAccessorOptions {
  bool export_ignore = false;
  bool smudgeLfs = false;
  bool submodules = false; // Currently implemented in GitInputScheme rather than GitAccessor

  std::string makeFingerprint(const Hash& rev) const;
};

struct GitRepo {
  virtual ~GitRepo() {}

  struct Options {
    bool create = false;
    bool bare = false;
    bool packfilesOnly = false;
  };

  static ref<GitRepo> openRepo(const std::filesystem::path& path, Options options);

  virtual uint64_t get_rev_count(const Hash& rev) = 0;

  virtual uint64_t get_last_modified(const Hash& rev) = 0;

  virtual bool is_shallow() = 0;

  /* Return the commit hash to which a ref points. */
  virtual Hash resolveRef(std::string ref) = 0;

  virtual void setRemote(const std::string& name, const std::string& url) = 0;

  /**
   * Info about a submodule.
   */
  struct submodule_t {
    canon_path_t path;
    std::string url;
    std::string branch;
  };

  struct WorkdirInfo {
    bool isDirty = false;

    /* The checked out commit, or nullopt if there are no commits
       in the repo yet. */
    std::optional<Hash> headRev;

    /* All files in the working directory that are unchanged,
       modified or added, but excluding deleted files. */
    std::set<canon_path_t> files;

    /* All modified or added files. */
    std::set<canon_path_t> dirtyFiles;

    /* The deleted files. */
    std::set<canon_path_t> deletedFiles;

    /* The submodules listed in .gitmodules of this workdir. */
    std::vector<submodule_t> submodules;
  };

  virtual WorkdirInfo getWorkdirInfo() = 0;

  static WorkdirInfo getCachedWorkdirInfo(const std::filesystem::path& path);

  /* Get the ref that HEAD points to. */
  virtual std::optional<std::string> getWorkdirRef() = 0;

  /**
   * Return the submodules of this repo at the indicated revision,
   * along with the revision of each submodule.
   */
  virtual std::vector<std::tuple<submodule_t, Hash>> getSubmodules(const Hash& rev,
                                                                   bool export_ignore) = 0;

  virtual std::string resolveSubmoduleUrl(const std::string& url) = 0;

  virtual bool hasObject(const Hash& oid) = 0;

  /**
   * Check if a tree object and all its descendants (subtrees and blobs)
   * exist in the repository. This is a deep validation that ensures the
   * entire tree is complete, not just the root object.
   *
   * This is important for detecting corrupted/incomplete Git trees that
   * can result from interrupted fetches or shallow clones.
   */
  virtual bool hasCompleteTree(const Hash& oid) = 0;

  virtual ref<source_accessor_t> get_accessor(const Hash& rev, const GitAccessorOptions& options,
                                              std::string display_prefix) = 0;

  virtual ref<source_accessor_t> get_accessor(const WorkdirInfo& wd,
                                              const GitAccessorOptions& options,
                                              MakeNotAllowedError make_not_allowed_error) = 0;

  virtual ref<GitFileSystemObjectSink> get_file_system_object_sink() = 0;

  virtual void flush() = 0;

  virtual void fetch(const std::string& url, const std::string& refspec, bool shallow) = 0;

  /**
   * Verify that commit `rev` is signed by one of the keys in
   * `public_keys`. Throw an error if it isn't.
   */
  virtual void verify_commit(const Hash& rev,
                             const std::vector<fetchers::public_key_t>& public_keys) = 0;

  /**
   * Given a git tree hash, compute the hash of its NAR
   * serialisation. This is memoised on-disk.
   */
  virtual Hash treeHashToNarHash(const fetchers::settings_t& settings, const Hash& tree_hash) = 0;

  /**
   * If the specified git object is a directory with a single entry
   * that is a directory, return the ID of that object.
   * Otherwise, return the passed ID unchanged.
   */
  virtual Hash dereferenceSingletonDirectory(const Hash& oid) = 0;
};

// A helper to ensure that the `git_*_free` functions get called.
template <auto del>
struct Deleter {
  template <typename T>
  void operator()(T* p) const {
    del(p);
  };
};

// A helper to ensure that we don't leak objects returned by libgit2.
template <typename T>
struct Setter {
  T& t;
  typename T::pointer p = nullptr;

  Setter(T& t) : t(t) {}

  ~Setter() {
    if (p)
      t = T(p);
  }

  operator typename T::pointer *() { return &p; }
};

/**
 * Checks that the string can be a valid git reference, branch or tag name.
 * Accepts shorthand references (one-level refnames are allowed), pseudorefs
 * like `HEAD`.
 *
 * @note This is a coarse test to make sure that the refname is at least something
 * that git can make sense of.
 */
bool is_legal_ref_name(const std::string& ref_name);

} // namespace nix
