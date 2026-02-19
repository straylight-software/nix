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

std::optional<Mode> decode_mode(raw_mode_t m) {
  switch (m) {
    case (raw_mode_t)Mode::directory_t:
    case (raw_mode_t)Mode::executable:
    case (raw_mode_t)Mode::regular:
    case (raw_mode_t)Mode::symlink:
      return (Mode)m;
    default:
      return std::nullopt;
  }
}

static std::string get_string_until(Source& source, char byte) {
  std::string s;
  char n[1] = {0};
  source(std::string_view{n, 1});
  while (*n != byte) {
    s += *n;
    source(std::string_view{n, 1});
  }
  return s;
}

static std::string get_string(Source& source, int n) {
  std::string v;
  v.resize(n);
  source(v);
  return v;
}

void parse_blob(file_system_object_sink_t& sink, const canon_path_t& sink_path, Source& source,
               blob_mode_t blob_mode, const experimental_feature_settings_t& xp_settings) {
  xp_settings.require(xp_t::git_hashing);

  const unsigned long long size = std::stoi(get_string_until(source, 0));

  auto do_regular_file = [&](bool executable) {
    sink.create_regular_file(sink_path, [&](auto& crf) {
      if (executable)
        crf.is_executable();

      crf.preallocate_contents(size);

      unsigned long long left = size;
      std::string buf;
      buf.reserve(65536);

      while (left) {
        check_interrupt();
        buf.resize(std::min((unsigned long long)buf.capacity(), left));
        source(buf);
        crf(buf);
        left -= buf.size();
      }
    });
  };

  switch (blob_mode) {
    case blob_mode_t::regular:
      do_regular_file(false);
      break;

    case blob_mode_t::executable:
      do_regular_file(true);
      break;

    case blob_mode_t::symlink: {
      std::string target;
      target.resize(size, '0');
      target.reserve(size);
      for (size_t n = 0; n < target.size();) {
        check_interrupt();
        n += source.read(const_cast<char*>(target.c_str()) + n, target.size() - n);
      }

      sink.create_symlink(sink_path, target);
      break;
    }

    default:
      assert(false);
  }
}

void parse_tree(file_system_object_sink_t& sink, const canon_path_t& sink_path, Source& source,
               hash_algorithm_t hash_algo, std::function<sink_hook_t> hook,
               const experimental_feature_settings_t& xp_settings) {
  const unsigned long long size = std::stoi(get_string_until(source, 0));
  unsigned long long left = size;

  sink.create_directory(sink_path);

  while (left) {
    std::string perms = get_string_until(source, ' ');
    left -= perms.size();
    left -= 1;

    raw_mode_t raw_mode = std::stoi(perms, 0, 8);
    auto mode_opt = decode_mode(raw_mode);
    if (!mode_opt)
      throw Error("Unknown Git permission: %o", raw_mode);
    auto mode = std::move(*mode_opt);

    std::string name = get_string_until(source, '\0');
    left -= name.size();
    left -= 1;

    const auto hash_size = regular_hash_size(hash_algo);
    std::string hashs = get_string(source, hash_size);
    left -= hash_size;

    if (!(hash_algo == hash_algorithm_t::SHA1 || hash_algo == hash_algorithm_t::SHA256)) {
      throw Error("Unsupported hash algorithm for git trees: %s", print_hash_algo(hash_algo));
    }

    Hash hash(hash_algo);
    std::copy(hashs.begin(), hashs.end(), hash.hash);

    hook(canon_path_t{name}, tree_entry{
                              .mode = mode,
                              .hash = hash,
                          });
  }
}

object_type_t parse_object_type(Source& source, const experimental_feature_settings_t& xp_settings) {
  xp_settings.require(xp_t::git_hashing);

  auto type = get_string(source, 5);

  if (type == "blob ") {
    return object_type_t::blob;
  } else if (type == "tree ") {
    return object_type_t::tree_t;
  } else
    throw Error("input doesn't look like a Git object");
}

void parse(file_system_object_sink_t& sink, const canon_path_t& sink_path, Source& source,
           blob_mode_t root_mode_if_blob, hash_algorithm_t hash_algo, std::function<sink_hook_t> hook,
           const experimental_feature_settings_t& xp_settings) {
  xp_settings.require(xp_t::git_hashing);

  auto type = parse_object_type(source, xp_settings);

  switch (type) {
    case object_type_t::blob:
      parse_blob(sink, sink_path, source, root_mode_if_blob, xp_settings);
      break;
    case object_type_t::tree_t:
      parse_tree(sink, sink_path, source, hash_algo, hook, xp_settings);
      break;
    default:
      assert(false);
  };
}

std::optional<Mode> convert_mode(SourceAccessor::Type type) {
  switch (type) {
    case SourceAccessor::t_symlink:
      return Mode::symlink;
    case SourceAccessor::t_regular:
      return Mode::regular;
    case SourceAccessor::t_directory:
      return Mode::directory_t;
    case SourceAccessor::t_char:
    case SourceAccessor::t_block:
    case SourceAccessor::t_socket:
    case SourceAccessor::t_fifo:
      return std::nullopt;
    case SourceAccessor::t_unknown:
    default:
      unreachable();
  }
}

void restore(file_system_object_sink_t& sink, Source& source, hash_algorithm_t hash_algo,
             std::function<restore_hook_t> hook) {
  parse(sink, canon_path_t::root, source, blob_mode_t::regular, hash_algo,
        [&](canon_path_t name, tree_entry entry) {
          auto [accessor, from] = hook(entry.hash);
          auto stat = accessor->lstat(from);
          auto got_opt = convert_mode(stat.type);
          if (!got_opt)
            throw Error("file '%s' (git hash %s) has an unsupported type", from,
                        entry.hash.to_string(hash_format_t::base16, false));
          auto& got = *got_opt;
          if (got != entry.mode)
            throw Error("git mode of file '%s' (git hash %s) is %o but expected %o", from,
                        entry.hash.to_string(hash_format_t::base16, false), (raw_mode_t)got,
                        (raw_mode_t)entry.mode);
          copy_recursive(*accessor, from, sink, name);
        });
}

void dump_blob_prefix(uint64_t size, Sink& sink, const experimental_feature_settings_t& xp_settings) {
  xp_settings.require(xp_t::git_hashing);
  auto s = fmt("blob %d\0"s, std::to_string(size));
  sink(s);
}

void dump_tree(const tree_t& entries, Sink& sink, const experimental_feature_settings_t& xp_settings) {
  xp_settings.require(xp_t::git_hashing);

  std::string v1;

  for (auto& [name, entry] : entries) {
    auto name2 = name;
    if (entry.mode == Mode::directory_t) {
      assert(!name2.empty());
      assert(name2.back() == '/');
      name2.pop_back();
    }
    v1 += fmt("%o %s\0"s, static_cast<raw_mode_t>(entry.mode), name2);
    std::copy(entry.hash.hash, entry.hash.hash + entry.hash.hash_size, std::back_inserter(v1));
  }

  {
    auto s = fmt("tree %d\0"s, v1.size());
    sink(s);
  }

  sink(v1);
}

Mode dump(const source_path_t& path, Sink& sink, std::function<dump_hook_t> hook, path_filter_t& filter,
          const experimental_feature_settings_t& xp_settings) {
  auto st = path.lstat();

  switch (st.type) {
    case SourceAccessor::t_regular: {
      path.read_file(sink, [&](uint64_t size) { dump_blob_prefix(size, sink, xp_settings); });
      return st.is_executable ? Mode::executable : Mode::regular;
    }

    case SourceAccessor::t_directory: {
      tree_t entries;
      for (auto& [name, _] : path.read_directory()) {
        auto child = path / name;
        if (!filter(child.path.abs()))
          continue;

        auto entry = hook(child);

        auto name2 = name;
        if (entry.mode == Mode::directory_t)
          name2 += "/";

        entries.insert_or_assign(std::move(name2), std::move(entry));
      }
      dump_tree(entries, sink, xp_settings);
      return Mode::directory_t;
    }

    case SourceAccessor::t_symlink: {
      auto target = path.read_link();
      dump_blob_prefix(target.size(), sink, xp_settings);
      sink(target);
      return Mode::symlink;
    }

    case SourceAccessor::t_char:
    case SourceAccessor::t_block:
    case SourceAccessor::t_socket:
    case SourceAccessor::t_fifo:
    case SourceAccessor::t_unknown:
    default:
      throw Error("file '%1%' has an unsupported type of %2%", path, st.type_string());
  }
}

tree_entry dump_hash(hash_algorithm_t ha, const source_path_t& path, path_filter_t& filter) {
  std::function<dump_hook_t> hook;
  hook = [&](const source_path_t& path) -> tree_entry {
    auto hash_sink = hash_sink_t(ha);
    auto mode = dump(path, hash_sink, hook, filter);
    auto hash = hash_sink.finish().hash;
    return {
        .mode = mode,
        .hash = hash,
    };
  };

  return hook(path);
}

std::optional<ls_remote_ref_line_t> parse_ls_remote_line(std::string_view line) {
  const static std::regex line_regex("^(ref: *)?([^\\s]+)(?:\\t+(.*))?$");
  std::match_results<std::string_view::const_iterator> match;
  if (!std::regex_match(line.cbegin(), line.cend(), match, line_regex))
    return std::nullopt;

  return ls_remote_ref_line_t{
      .kind =
          match[1].length() == 0 ? ls_remote_ref_line_t::Kind::Object : ls_remote_ref_line_t::Kind::symbolic,
      .target = match[2],
      .reference = match[3].length() == 0 ? std::nullopt : std::optional<std::string>{match[3]}};
}

} // namespace nix::git
