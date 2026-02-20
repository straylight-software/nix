#pragma once
///@file

#include <boost/unordered/unordered_flat_map.hpp>
#include <boost/unordered/unordered_flat_set.hpp>

#include "nix/store/store-api.h"

namespace nix {

// FIXME: should turn this into an std::variant to represent the
// several root types.
using GcRootInfo = std::string;

typedef boost::unordered_flat_map<
    store_path_t, boost::unordered_flat_set<GcRootInfo, string_view_hash_t, std::equal_to<>>,
    std::hash<store_path_t>>
    Roots;

/**
 * Garbage collector operation:
 *
 * - `gcReturnLive`: return the set of paths reachable from
 *   (i.e. in the closure of) the roots.
 *
 * - `gcReturnDead`: return the set of paths not reachable from
 *   the roots.
 *
 * - `gcDeleteDead`: actually delete the latter set.
 *
 * - `gcDeleteSpecific`: delete the paths listed in
 *    `pathsToDelete`, insofar as they are not reachable.
 */
enum class GCAction {
  gcReturnLive,
  gcReturnDead,
  gcDeleteDead,
  gcDeleteSpecific,
};

struct GCOptions {
  using GCAction = nix::GCAction;
  using enum GCAction;

  GCAction action{gcDeleteDead};

  /**
   * If `ignoreLiveness` is set, then reachability from the roots is
   * ignored (dangerous!).  However, the paths must still be
   * unreferenced *within* the store (i.e., there can be no other
   * store paths that depend on them).
   */
  bool ignoreLiveness{false};

  /**
   * For `gcDeleteSpecific`, the paths to delete.
   */
  store_path_set_t pathsToDelete;

  /**
   * Stop after at least `maxFreed` bytes have been freed.
   */
  uint64_t maxFreed{std::numeric_limits<uint64_t>::max()};

  /**
   * Whether to hide potentially sensitive information about GC
   * roots (such as PIDs).
   */
  bool censor = false;
};

struct GCResults {
  /**
   * Depending on the action, the GC roots, or the paths that would
   * be or have been deleted.
   */
  path_set_t paths;

  /**
   * For `gcReturnDead`, `gcDeleteDead` and `gcDeleteSpecific`, the
   * number of bytes that would be or was freed.
   */
  uint64_t bytes_freed = 0;
};

/**
 * Mix-in class for \ref store_t "stores" which expose a notion of garbage
 * collection.
 *
 * Garbage collection will allow deleting paths which are not
 * transitively "rooted".
 *
 * The notion of GC roots actually not part of this class.
 *
 *  - The base `store_t` class has `store_t::addTempRoot()` because for a store
 *    that doesn't support garbage collection at all, a temporary GC root is
 *    safely implementable as no-op.
 *
 *    @todo actually this is not so good because stores are *views*.
 *    Some views have only a no-op temp roots even though others to the
 *    same store allow triggering GC. For instance one can't add a root
 *    over ssh, but that doesn't prevent someone from gc-ing that store
 *    accessed via SSH locally).
 *
 *  - The derived `local_fs_store` class has `local_fs_store::addPermRoot`,
 *    which is not part of this class because it relies on the notion of
 *    an ambient file system. There are stores (`ssh-ng://`, for one),
 *    that *do* support garbage collection but *don't* expose any file
 *    system, and `local_fs_store::addPermRoot` thus does not make sense
 *    for them.
 */
struct GcStore : public virtual store_t {
  inline static std::string operation_name = "Garbage collection";

  /**
   * Find the roots of the garbage collector.  Each root is a pair
   * `(link, storepath)` where `link` is the path of the symlink
   * outside of the Nix store that point to `store_path`. If
   * `censor` is true, privacy-sensitive information about roots
   * found in `/proc` is censored.
   */
  virtual Roots findRoots(bool censor) = 0;

  /**
   * Perform a garbage collection.
   */
  virtual void collectGarbage(const GCOptions& options, GCResults& results) = 0;
};

} // namespace nix
