#include "nix/store/indirect-root-store.h"

namespace nix {

void IndirectRootStore::makeSymlink(const Path& link, const Path& target) {
  /* Create directories up to `gc_root'. */
  create_dirs(dir_of(link));

  /* Create the new symlink. */
  Path tempLink = fmt("%1%.tmp-%2%-%3%", link, getpid(), rand());
  create_symlink(target, tempLink);

  /* Atomically replace the old one. */
  std::filesystem::rename(tempLink, link);
}

Path IndirectRootStore::addPermRoot(const StorePath& store_path, const Path& _gcRoot) {
  Path gc_root(canon_path(_gcRoot));

  if (isInStore(gc_root))
    throw Error("creating a garbage collector root (%1%) in the Nix store is forbidden "
                "(are you running nix-build inside the store?)",
                gc_root);

  /* Register this root with the garbage collector, if it's
     running. This should be superfluous since the caller should
     have registered this root yet, but let's be on the safe
     side. */
  addTempRoot(store_path);

  /* Don't clobber the link if it already exists and doesn't
     point to the Nix store. */
  if (path_exists(gc_root) && (!std::filesystem::is_symlink(gc_root) || !isInStore(read_link(gc_root))))
    throw Error("cannot create symlink '%1%'; already exists", gc_root);

  makeSymlink(gc_root, printStorePath(store_path));
  addIndirectRoot(gc_root);

  return gc_root;
}

} // namespace nix
