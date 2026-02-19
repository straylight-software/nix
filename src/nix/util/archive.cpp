#include "nix/util/archive.h"

#include <algorithm>
#include <cerrno>
#include <map>
#include <vector>

#include <strings.h> // for strcasecmp

#include "nix/util/alignment.h"
#include "nix/util/config-global.h"
#include "nix/util/file-system.h"
#include "nix/util/posix-source-accessor.h"
#include "nix/util/signals.h"
#include "nix/util/source-path.h"

namespace nix {

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

static archive_settings_t archive_settings;

static global_config_t::Register r_archive_settings(&archive_settings);

path_filter_t default_path_filter = [](const Path&) { return true; };

void SourceAccessor::dump_path(const canon_path_t& path, Sink& sink, path_filter_t& filter) {
  auto dump_contents = [&](const canon_path_t& path) {
    sink << "contents";
    std::optional<uint64_t> size;
    read_file(path, sink, [&](uint64_t _size) {
      size = _size;
      sink << _size;
    });
    assert(size);
    write_padding(*size, sink);
  };

  sink << nar_version_magic1;

  [&, &this_(*this)](this const auto& dump, const canon_path_t& path) -> void {
    check_interrupt();

    auto st = this_.lstat(path);

    sink << "(";

    if (st.type == t_regular) {
      sink << "type" << "regular";
      if (st.is_executable) {
        sink << "executable" << "";
}
      dump_contents(path);
    }

    else if (st.type == t_directory) {
      sink << "type" << "directory";

      /* If we're on a case-insensitive system like macOS, undo
         the case hack applied by restore_path(). */
      string_map_t unhacked;
      for (auto& i : this_.read_directory(path)) {
        if (archive_settings.use_case_hack) {
          std::string name(i.first);
          size_t pos = i.first.find(case_hack_suffix);
          if (pos != std::string::npos) {
            debug("removing case hack suffix from '%s'", path / i.first);
            name.erase(pos);
          }
          if (!unhacked.emplace(name, i.first).second) {
            throw Error("file name collision between '%s' and '%s'", (path / unhacked[name]),
                        (path / i.first));
}
        } else {
          unhacked.emplace(i.first, i.first);
}
}

      for (auto& i : unhacked) {
        if (filter((path / i.first).abs())) {
          sink << "entry" << "(" << "name" << i.first << "node";
          dump(path / i.second);
          sink << ")";
        }
}
    }

    else if (st.type == t_symlink) {
      sink << "type" << "symlink" << "target" << this_.read_link(path);

    } else {
      throw Error("file '%s' has an unsupported type", path);
}

    sink << ")";
  }(path);
}

time_t dump_path_and_get_mtime(const Path& path, Sink& sink, path_filter_t& filter) {
  auto path2 = posix_source_accessor_t::create_at_root(path, /*track_last_modified=*/true);
  path2.dump_path(sink, filter);
  return path2.accessor->get_last_modified().value();
}

void dump_path(const Path& path, Sink& sink, path_filter_t& filter) {
  dump_path_and_get_mtime(path, sink, filter);
}

void dump_string(std::string_view s, Sink& sink) {
  sink << nar_version_magic1 << "(" << "type" << "regular" << "contents" << s << ")";
}

template <typename... Args>
static SerialisationError bad_archive(std::string_view s, const Args&... args) {
  return SerialisationError("bad archive: " + s, args...);
}

static void parse_contents(create_regular_file_sink_t& sink, Source& source) {
  uint64_t size = read_long_long(source);

  sink.preallocate_contents(size);

  if (sink.skip_contents) {
    source.skip(align_up(size, 8));
    return;
  }

  uint64_t left = size;
  std::array<char, 65536> buf;

  while (left) {
    check_interrupt();
    auto n = buf.size();
    if ((uint64_t)n > left) {
      n = left;
}
    source(buf.data(), n);
    sink({buf.data(), n});
    left -= n;
  }

  read_padding(size, source);
}

struct case_insensitive_compare_t {
  bool operator()(const std::string& a, const std::string& b) const {
    return strcasecmp(a.c_str(), b.c_str()) < 0;
  }
};

static void parse(file_system_object_sink_t& sink, Source& source, const canon_path_t& path) {
  auto get_string = [&]() {
    check_interrupt();
    return read_string(source);
  };

  auto expect_tag = [&](std::string_view expected) {
    auto tag = get_string();
    if (tag != expected) {
      throw bad_archive("expected tag '%s', got '%s'", expected, tag.substr(0, 1024));
}
  };

  expect_tag("(");

  expect_tag("type");

  auto type = get_string();

  if (type == "regular") {
    sink.create_regular_file(path, [&](auto& crf) {
      auto tag = get_string();

      if (tag == "executable") {
        auto s2 = get_string();
        if (s2 != "") {
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
    sink.create_directory(path, [&](file_system_object_sink_t& dir_sink, const canon_path_t& rel_dir_path) {
      std::map<Path, int, case_insensitive_compare_t> names;

      std::string prev_name;

      while (1) {
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
        if (name.empty() || name == "." || name == ".." || name.find('/') != std::string::npos ||
            name.find((char)0) != std::string::npos) {
          throw bad_archive("NAR contains invalid file name '%1%'", name);
}
        if (name <= prev_name) {
          throw bad_archive("NAR directory is not sorted");
}
        prev_name = name;
        if (archive_settings.use_case_hack) {
          auto i = names.find(name);
          if (i != names.end()) {
            debug("case collision between '%1%' and '%2%'", i->first, name);
            name += case_hack_suffix;
            name += std::to_string(++i->second);
            auto j = names.find(name);
            if (j != names.end()) {
              throw bad_archive(
                  "NAR contains file name '%s' that collides with case-hacked file name '%s'",
                  prev_name, j->first);
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

void parse_dump(file_system_object_sink_t& sink, Source& source) {
  std::string version;
  try {
    version = read_string(source, nar_version_magic1.size());
  } catch (SerialisationError& e) {
    /* This generally means the integer at the start couldn't be
       decoded.  Ignore and throw the exception below. */
  }
  if (version != nar_version_magic1) {
    throw bad_archive("input doesn't look like a Nix archive");
}
  parse(sink, source, canon_path_t::root);
}

void restore_path(const std::filesystem::path& path, Source& source, bool start_fsync) {
  restore_sink_t sink{start_fsync};
  sink.dst_path = path;
  parse_dump(sink, source);
}

void copy_nar(Source& source, Sink& sink) {
  // FIXME: if 'source' is the output of dumpPath() followed by EOF,
  // we should just forward all data directly without parsing.

  null_file_system_object_sink_t parse_sink; /* just parse the NAR */

  tee_source_t wrapper{source, sink};

  parse_dump(parse_sink, wrapper);
}

} // namespace nix
