#pragma once
///@file

#include <sys/stat.h>
#include <sys/time.h>

#include "nix/util/error.h"
#include "nix/util/types.h"

namespace nix {

using Inode = std::pair<dev_t, ino_t>;
using InodesSeen = std::set<Inode>;

/**
 * "Fix", or canonicalise, the meta-data of the files in a store path
 * after it has been built.  In particular:
 *
 * - the last modification date on each file is set to 1 (i.e.,
 *   00:00:01 1/1/1970 UTC)
 *
 * - the permissions are set of 444 or 555 (i.e., read-only with or
 *   without execute permission; setuid bits etc. are cleared)
 *
 * - the owner and group are set to the Nix user and group, if we're
 *   running as root. (Unix only.)
 *
 * If uidRange is not empty, this function will throw an error if it
 * encounters files owned by a user outside of the closed interval
 * [uidRange->first, uidRange->second].
 */
void canonicalise_path_meta_data(const Path& path,
#ifndef _WIN32
                              std::optional<std::pair<uid_t, uid_t>> uidRange,
#endif
                              InodesSeen& inodes_seen);

void canonicalise_path_meta_data(const Path& path
#ifndef _WIN32
                              ,
                              std::optional<std::pair<uid_t, uid_t>> uidRange = std::nullopt
#endif
);

void canonicalise_timestamp_and_permissions(const Path& path);

make_error(PathInUse, Error);

} // namespace nix
