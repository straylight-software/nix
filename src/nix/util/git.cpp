#include "nix/util/git.h"

#include <algorithm>
#include <cerrno>
#include <map>
#include <regex>
#include <vector>

#include <strings.h> // for strcasecmp

#include "nix/util/configuration.h"
#include "nix/util/hash.h"
#include "nix/util/serialise.h"
#include "nix/util/signals.h"

namespace nix::git {

using namespace nix;
using namespace std::string_literals;

std::optional<Mode> decodeMode(raw_mode_t m) {
  switch (m) {
    case (raw_mode_t)Mode::directory_t:
    case (raw_mode_t)Mode::Executable:
    case (raw_mode_t)Mode::Regular:
    case (raw_mode_t)Mode::Symlink:
      return (Mode)m;
    default:
      return std::nullopt;
  }
}

static std::string getStringUntil(Source& source, char byte) {
  std::string s;
  char n[1] = {0};
  source(std::string_view{n, 1});
  while (*n != byte) {
    s += *n;
    source(std::string_view{n, 1});
  }
  return s;
}

static std::string getString(Source& source, int n) {
  std::string v;
  v.resize(n);
  source(v);
  return v;
}

void parseBlob(file_system_object_sink_t& sink, const canon_path_t& sinkPath, Source& source,
               blob_mode_t blobMode, const experimental_feature_settings_t& xpSettings) {
  xpSettings.require(xp_t::GitHashing);

  const unsigned long long size = std::stoi(getStringUntil(source, 0));

  auto doRegularFile = [&](bool executable) {
    sink.createRegularFile(sinkPath, [&](auto& crf) {
      if (executable)
        crf.isExecutable();

      crf.preallocateContents(size);

      unsigned long long left = size;
      std::string buf;
      buf.reserve(65536);

      while (left) {
        checkInterrupt();
        buf.resize(std::min((unsigned long long)buf.capacity(), left));
        source(buf);
        crf(buf);
        left -= buf.size();
      }
    });
  };

  switch (blobMode) {
    case blob_mode_t::Regular:
      doRegularFile(false);
      break;

    case blob_mode_t::Executable:
      doRegularFile(true);
      break;

    case blob_mode_t::Symlink: {
      std::string target;
      target.resize(size, '0');
      target.reserve(size);
      for (size_t n = 0; n < target.size();) {
        checkInterrupt();
        n += source.read(const_cast<char*>(target.c_str()) + n, target.size() - n);
      }

      sink.createSymlink(sinkPath, target);
      break;
    }

    default:
      assert(false);
  }
}

void parseTree(file_system_object_sink_t& sink, const canon_path_t& sinkPath, Source& source,
               hash_algorithm_t hashAlgo, std::function<sink_hook_t> hook,
               const experimental_feature_settings_t& xpSettings) {
  const unsigned long long size = std::stoi(getStringUntil(source, 0));
  unsigned long long left = size;

  sink.createDirectory(sinkPath);

  while (left) {
    std::string perms = getStringUntil(source, ' ');
    left -= perms.size();
    left -= 1;

    raw_mode_t rawMode = std::stoi(perms, 0, 8);
    auto modeOpt = decodeMode(rawMode);
    if (!modeOpt)
      throw Error("Unknown Git permission: %o", rawMode);
    auto mode = std::move(*modeOpt);

    std::string name = getStringUntil(source, '\0');
    left -= name.size();
    left -= 1;

    const auto hashSize = regularHashSize(hashAlgo);
    std::string hashs = getString(source, hashSize);
    left -= hashSize;

    if (!(hashAlgo == hash_algorithm_t::SHA1 || hashAlgo == hash_algorithm_t::SHA256)) {
      throw Error("Unsupported hash algorithm for git trees: %s", printHashAlgo(hashAlgo));
    }

    Hash hash(hashAlgo);
    std::copy(hashs.begin(), hashs.end(), hash.hash);

    hook(canon_path_t{name}, TreeEntry{
                              .mode = mode,
                              .hash = hash,
                          });
  }
}

object_type_t parseObjectType(Source& source, const experimental_feature_settings_t& xpSettings) {
  xpSettings.require(xp_t::GitHashing);

  auto type = getString(source, 5);

  if (type == "blob ") {
    return object_type_t::Blob;
  } else if (type == "tree ") {
    return object_type_t::tree_t;
  } else
    throw Error("input doesn't look like a Git object");
}

void parse(file_system_object_sink_t& sink, const canon_path_t& sinkPath, Source& source,
           blob_mode_t rootModeIfBlob, hash_algorithm_t hashAlgo, std::function<sink_hook_t> hook,
           const experimental_feature_settings_t& xpSettings) {
  xpSettings.require(xp_t::GitHashing);

  auto type = parseObjectType(source, xpSettings);

  switch (type) {
    case object_type_t::Blob:
      parseBlob(sink, sinkPath, source, rootModeIfBlob, xpSettings);
      break;
    case object_type_t::tree_t:
      parseTree(sink, sinkPath, source, hashAlgo, hook, xpSettings);
      break;
    default:
      assert(false);
  };
}

std::optional<Mode> convertMode(SourceAccessor::Type type) {
  switch (type) {
    case SourceAccessor::tSymlink:
      return Mode::Symlink;
    case SourceAccessor::tRegular:
      return Mode::Regular;
    case SourceAccessor::tDirectory:
      return Mode::directory_t;
    case SourceAccessor::tChar:
    case SourceAccessor::tBlock:
    case SourceAccessor::tSocket:
    case SourceAccessor::tFifo:
      return std::nullopt;
    case SourceAccessor::tUnknown:
    default:
      unreachable();
  }
}

void restore(file_system_object_sink_t& sink, Source& source, hash_algorithm_t hashAlgo,
             std::function<restore_hook_t> hook) {
  parse(sink, canon_path_t::root, source, blob_mode_t::Regular, hashAlgo,
        [&](canon_path_t name, TreeEntry entry) {
          auto [accessor, from] = hook(entry.hash);
          auto stat = accessor->lstat(from);
          auto gotOpt = convertMode(stat.type);
          if (!gotOpt)
            throw Error("file '%s' (git hash %s) has an unsupported type", from,
                        entry.hash.to_string(hash_format_t::Base16, false));
          auto& got = *gotOpt;
          if (got != entry.mode)
            throw Error("git mode of file '%s' (git hash %s) is %o but expected %o", from,
                        entry.hash.to_string(hash_format_t::Base16, false), (raw_mode_t)got,
                        (raw_mode_t)entry.mode);
          copyRecursive(*accessor, from, sink, name);
        });
}

void dumpBlobPrefix(uint64_t size, Sink& sink, const experimental_feature_settings_t& xpSettings) {
  xpSettings.require(xp_t::GitHashing);
  auto s = fmt("blob %d\0"s, std::to_string(size));
  sink(s);
}

void dumpTree(const tree_t& entries, Sink& sink, const experimental_feature_settings_t& xpSettings) {
  xpSettings.require(xp_t::GitHashing);

  std::string v1;

  for (auto& [name, entry] : entries) {
    auto name2 = name;
    if (entry.mode == Mode::directory_t) {
      assert(!name2.empty());
      assert(name2.back() == '/');
      name2.pop_back();
    }
    v1 += fmt("%o %s\0"s, static_cast<raw_mode_t>(entry.mode), name2);
    std::copy(entry.hash.hash, entry.hash.hash + entry.hash.hashSize, std::back_inserter(v1));
  }

  {
    auto s = fmt("tree %d\0"s, v1.size());
    sink(s);
  }

  sink(v1);
}

Mode dump(const source_path_t& path, Sink& sink, std::function<dump_hook_t> hook, path_filter_t& filter,
          const experimental_feature_settings_t& xpSettings) {
  auto st = path.lstat();

  switch (st.type) {
    case SourceAccessor::tRegular: {
      path.readFile(sink, [&](uint64_t size) { dumpBlobPrefix(size, sink, xpSettings); });
      return st.isExecutable ? Mode::Executable : Mode::Regular;
    }

    case SourceAccessor::tDirectory: {
      tree_t entries;
      for (auto& [name, _] : path.readDirectory()) {
        auto child = path / name;
        if (!filter(child.path.abs()))
          continue;

        auto entry = hook(child);

        auto name2 = name;
        if (entry.mode == Mode::directory_t)
          name2 += "/";

        entries.insert_or_assign(std::move(name2), std::move(entry));
      }
      dumpTree(entries, sink, xpSettings);
      return Mode::directory_t;
    }

    case SourceAccessor::tSymlink: {
      auto target = path.readLink();
      dumpBlobPrefix(target.size(), sink, xpSettings);
      sink(target);
      return Mode::Symlink;
    }

    case SourceAccessor::tChar:
    case SourceAccessor::tBlock:
    case SourceAccessor::tSocket:
    case SourceAccessor::tFifo:
    case SourceAccessor::tUnknown:
    default:
      throw Error("file '%1%' has an unsupported type of %2%", path, st.typeString());
  }
}

TreeEntry dumpHash(hash_algorithm_t ha, const source_path_t& path, path_filter_t& filter) {
  std::function<dump_hook_t> hook;
  hook = [&](const source_path_t& path) -> TreeEntry {
    auto hashSink = hash_sink_t(ha);
    auto mode = dump(path, hashSink, hook, filter);
    auto hash = hashSink.finish().hash;
    return {
        .mode = mode,
        .hash = hash,
    };
  };

  return hook(path);
}

std::optional<ls_remote_ref_line_t> parseLsRemoteLine(std::string_view line) {
  const static std::regex line_regex("^(ref: *)?([^\\s]+)(?:\\t+(.*))?$");
  std::match_results<std::string_view::const_iterator> match;
  if (!std::regex_match(line.cbegin(), line.cend(), match, line_regex))
    return std::nullopt;

  return ls_remote_ref_line_t{
      .kind =
          match[1].length() == 0 ? ls_remote_ref_line_t::Kind::Object : ls_remote_ref_line_t::Kind::Symbolic,
      .target = match[2],
      .reference = match[3].length() == 0 ? std::nullopt : std::optional<std::string>{match[3]}};
}

} // namespace nix::git
