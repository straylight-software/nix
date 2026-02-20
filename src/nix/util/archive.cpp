#include "nix/util/archive.h"

#include <algorithm>
#include <cerrno>
#include <map>
#include <vector>

#include <strings.h> // for strcasecmp

#include "nix/util/alignment.h"
#include "nix/util/canon-path.h"
#include "nix/util/config-global.h"
#include "nix/util/configuration.h"
#include "nix/util/file-system.h"
#include "nix/util/fs-sink.h"
#include "nix/util/logging.h"
#include "nix/util/posix-source-accessor.h"
#include "nix/util/serialise.h"
#include "nix/util/signals.h"
#include "nix/util/source-accessor.h"
#include "nix/util/source-path.h"

namespace nix {

namespace {

struct archive_settings_t : config_t {
  setting_t<bool> use_case_hack{
      this,
#ifdef __APPLE__
      true,
#else
      false,
#endif
      "use-case-hack",
      "Whether to enable a macOS-specific hack for dealing with file name case collisions."};
};

archive_settings_t archive_settings;

global_config_t::Register r_archive_settings(&archive_settings);

} // namespace

path_filter_t default_path_filter = [](const Path& /*path*/) -> bool { return true; };

void source_accessor_t::dump_path(const canon_path_t& path, nix::sink_t& s, path_filter_t& filter) {
  auto dump_contents = [&](const canon_path_t& p) -> void {
    s << "contents";
    std::optional<uint64_t> size;
    read_file(p, s, [&](uint64_t file_size) {
      size = file_size;
      s << file_size;
    });
    assert(size);
    write_padding(*size, s);
  };

  s << nar_version_magic1;

  [&, &this_(*this)](this const auto& dump, const canon_path_t& p) -> void {
    check_interrupt();

    auto stat = this_.lstat(p);

    s << "(";

    if (stat.type == t_regular) {
      s << "type" << "regular";
      if (stat.is_executable) {
        s << "executable" << "";
      }
      dump_contents(p);
    }

    else if (stat.type == t_directory) {
      s << "type" << "directory";

      /* If we're on a case-insensitive system like macOS, undo
         the case hack applied by restore_path(). */
      string_map_t unhacked;
      for (auto& entry : this_.read_directory(p)) {
        if (archive_settings.use_case_hack) {
          std::string name(entry.first);
          const std::size_t pos = entry.first.find(case_hack_suffix);
          if (pos != std::string::npos) {
            debug("removing case hack suffix from '%s'", p / entry.first);
            name.erase(pos);
          }
          if (!unhacked.emplace(name, entry.first).second) {
            throw Error("file name collision between '%s' and '%s'", (p / unhacked[name]),
                        (p / entry.first));
          }
        } else {
          unhacked.emplace(entry.first, entry.first);
        }
      }

      for (auto& entry : unhacked) {
        if (filter((p / entry.first).abs())) {
          s << "entry" << "(" << "name" << entry.first << "node";
          dump(p / entry.second);
          s << ")";
        }
      }
    }

    else if (stat.type == t_symlink) {
      s << "type" << "symlink" << "target" << this_.read_link(p);

    } else {
      throw Error("file '%s' has an unsupported type", p);
    }

    s << ")";
  }(path);
}

auto dump_path_and_get_mtime(const Path& path, nix::sink_t& s, path_filter_t& filter) -> time_t {
  auto path2 = posix_source_accessor_t::create_at_root(path, /*track_last_modified=*/true);
  path2.dump_path(s, filter);
  return path2.accessor->get_last_modified().value();
}

auto dump_path(const Path& path, nix::sink_t& s, path_filter_t& filter) -> void {
  (void)dump_path_and_get_mtime(path, s, filter);
}

auto dump_string(std::string_view str, nix::sink_t& s) -> void {
  s << nar_version_magic1 << "(" << "type" << "regular" << "contents" << str << ")";
}

namespace {

inline constexpr size_t k_alignment_bytes = 8U;
inline constexpr size_t k_buffer_size = 65536U;
inline constexpr size_t k_max_tag_display_length = 1024U;

template <typename... args_t>
auto bad_archive(std::string_view msg, const args_t&... args) -> SerialisationError {
  return SerialisationError("bad archive: " + msg, args...);
}

void parse_contents(create_regular_file_sink_t& sink, source_t& source) {
  const uint64_t size = read_long_long(source);

  sink.preallocate_contents(size);

  if (sink.skip_contents) {
    source.skip(align_up(size, k_alignment_bytes));
    return;
  }

  uint64_t left = size;
  std::array<char, k_buffer_size> buf{};

  while (left != 0U) {
    check_interrupt();
    auto count = buf.size();
    if (static_cast<uint64_t>(count) > left) {
      count = left;
    }
    source(buf.data(), count);
    sink({buf.data(), count});
    left -= count;
  }

  read_padding(size, source);
}

struct case_insensitive_compare_t {
  [[nodiscard]] auto operator()(const std::string& lhs, const std::string& rhs) const -> bool {
    return strcasecmp(lhs.c_str(), rhs.c_str()) < 0;
  }
};

void parse(file_system_object_sink_t& sink, source_t& source, const canon_path_t& path) {
  auto get_string = [&]() -> std::string {
    check_interrupt();
    return read_string(source);
  };

  auto expect_tag = [&](std::string_view expected) {
    auto tag = get_string();
    if (tag != expected) {
      throw bad_archive("expected tag '%s', got '%s'", expected,
                        tag.substr(0, k_max_tag_display_length));
    }
  };

  expect_tag("(");

  expect_tag("type");

  auto type = get_string();

  if (type == "regular") {
    sink.create_regular_file(path, [&](auto& crf) -> void {
      auto tag = get_string();

      if (tag == "executable") {
        auto exec_value = get_string();
        if (!exec_value.empty()) {
          throw bad_archive("executable marker has non-empty value");
        }
        crf.is_executable();
        tag = get_string();
      }

      if (tag != "contents") {
        throw bad_archive("expected tag 'contents', got '%s'", tag);
      }

      parse_contents(crf, source);

      expect_tag(")");
    });
  }

  else if (type == "directory") {
    sink.create_directory(
        path, [&](file_system_object_sink_t& dir_sink, const canon_path_t& rel_dir_path) -> void {
          std::map<Path, int, case_insensitive_compare_t> names;

          std::string prev_name;

          while (true) {
            auto tag = get_string();

            if (tag == ")") {
              break;
            }

            if (tag != "entry") {
              throw bad_archive("expected tag 'entry' or ')', got '%s'", tag);
            }

            expect_tag("(");

            expect_tag("name");

            auto name = get_string();
            if (name.empty() || name == "." || name == ".." ||
                name.find('/') != std::string::npos || name.find((char)0) != std::string::npos) {
              throw bad_archive("NAR contains invalid file name '%1%'", name);
            }
            if (name <= prev_name) {
              throw bad_archive("NAR directory is not sorted");
            }
            prev_name = name;
            if (archive_settings.use_case_hack) {
              auto iter = names.find(name);
              if (iter != names.end()) {
                debug("case collision between '%1%' and '%2%'", iter->first, name);
                name += case_hack_suffix;
                name += std::to_string(++iter->second);
                auto collision_iter = names.find(name);
                if (collision_iter != names.end()) {
                  throw bad_archive(
                      "NAR contains file name '%s' that collides with case-hacked file name '%s'",
                      prev_name, collision_iter->first);
                }
              } else {
                names[name] = 0;
              }
            }

            expect_tag("node");

            parse(dir_sink, source, rel_dir_path / name);

            expect_tag(")");
          }
        });
  }

  else if (type == "symlink") {
    expect_tag("target");

    auto target = get_string();
    sink.create_symlink(path, target);

    expect_tag(")");
  }

  else {
    throw bad_archive("unknown file type '%s'", type);
  }
}

} // namespace

auto parse_dump(nix::file_system_object_sink_t& fso_sink, nix::source_t& src) -> void {
  std::string version;
  try {
    version = read_string(src, nar_version_magic1.size());
  } catch (SerialisationError& e) {
    /* This generally means the integer at the start couldn't be
       decoded.  Ignore and throw the exception below. */
  }
  if (version != nar_version_magic1) {
    throw bad_archive("input doesn't look like a Nix archive");
  }
  parse(fso_sink, src, canon_path_t::root);
}

auto restore_path(const std::filesystem::path& path, nix::source_t& src, bool start_fsync) -> void {
  nix::restore_sink_t restore_sink{start_fsync};
  restore_sink.dst_path = path;
  parse_dump(restore_sink, src);
}

auto copy_nar(nix::source_t& src, nix::sink_t& s) -> void {
  // FIXME: if 'source' is the output of dumpPath() followed by EOF,
  // we should just forward all data directly without parsing.

  nix::null_file_system_object_sink_t parse_sink; /* just parse the NAR */

  nix::tee_source_t wrapper{src, s};

  parse_dump(parse_sink, wrapper);
}

} // namespace nix
