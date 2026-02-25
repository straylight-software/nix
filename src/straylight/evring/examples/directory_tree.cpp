// directory_tree.cpp - Recursive directory listing with statx
//
// This example demonstrates:
// - Using getdents64 (via syscall) for directory enumeration
// - Using statx for file metadata
// - Recursive tree traversal with state machine
// - Formatted tree output
//
// Usage: directory_tree [path]

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <unistd.h>

#include "straylight/evring/evring.h"

// ============================================================================
// Directory entry structure
// ============================================================================

struct dir_entry {
  std::string name;
  std::string path;
  bool is_directory{false};
  bool is_symlink{false};
  std::uint64_t size{0};
  mode_t mode{0};
};

// ============================================================================
// Format file size for display
// ============================================================================

auto format_size(std::uint64_t size) -> std::string {
  if (size < 1024) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%luB", size);
    return buf;
  } else if (size < 1024 * 1024) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%.1fK", static_cast<double>(size) / 1024);
    return buf;
  } else if (size < 1024 * 1024 * 1024) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%.1fM", static_cast<double>(size) / (1024 * 1024));
    return buf;
  } else {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%.1fG", static_cast<double>(size) / (1024 * 1024 * 1024));
    return buf;
  }
}

// ============================================================================
// Format mode for display
// ============================================================================

auto format_mode(mode_t mode) -> std::string {
  char buf[11];

  // File type
  if (S_ISDIR(mode))
    buf[0] = 'd';
  else if (S_ISLNK(mode))
    buf[0] = 'l';
  else if (S_ISREG(mode))
    buf[0] = '-';
  else if (S_ISBLK(mode))
    buf[0] = 'b';
  else if (S_ISCHR(mode))
    buf[0] = 'c';
  else if (S_ISFIFO(mode))
    buf[0] = 'p';
  else if (S_ISSOCK(mode))
    buf[0] = 's';
  else
    buf[0] = '?';

  // Owner permissions
  buf[1] = (mode & S_IRUSR) ? 'r' : '-';
  buf[2] = (mode & S_IWUSR) ? 'w' : '-';
  buf[3] = (mode & S_IXUSR) ? 'x' : '-';

  // Group permissions
  buf[4] = (mode & S_IRGRP) ? 'r' : '-';
  buf[5] = (mode & S_IWGRP) ? 'w' : '-';
  buf[6] = (mode & S_IXGRP) ? 'x' : '-';

  // Other permissions
  buf[7] = (mode & S_IROTH) ? 'r' : '-';
  buf[8] = (mode & S_IWOTH) ? 'w' : '-';
  buf[9] = (mode & S_IXOTH) ? 'x' : '-';

  buf[10] = '\0';
  return buf;
}

// ============================================================================
// Tree state machine - stat entries in a directory
// ============================================================================

struct stat_entries_state {
  enum class phase { initial, statting, done, error };

  phase current_phase{phase::initial};

  // Entries to stat
  std::vector<dir_entry> entries;
  std::size_t current_index{0};

  // Completed entries
  std::vector<dir_entry> completed;

  // Error info
  std::string error_message;
};

struct stat_entries_machine {
  using state_type = stat_entries_state;

  stat_entries_machine(std::vector<dir_entry> entries) : entries_(std::move(entries)) {}

  [[nodiscard]] auto initial() const -> state_type {
    state_type s;
    s.entries = entries_;
    return s;
  }

  [[nodiscard]] auto step(state_type s, const evring::event& e) const
      -> evring::step_result<state_type> {
    std::vector<evring::operation> ops;

    switch (s.current_phase) {
      case state_type::phase::initial:
        if (s.entries.empty()) {
          s.current_phase = state_type::phase::done;
        } else {
          s.current_phase = state_type::phase::statting;
          s.current_index = 0;
          ops.push_back(evring::operation::make_statx(
              AT_FDCWD, s.entries[0].path.c_str(), AT_SYMLINK_NOFOLLOW,
              STATX_TYPE | STATX_MODE | STATX_SIZE, evring::make_stable_ref(statx_buf_)));
        }
        break;

      case state_type::phase::statting: {
        // Process result (ignore errors - just mark entry as unknown)
        auto& entry = s.entries[s.current_index];
        if (e.ok()) {
          entry.mode = statx_buf_.stx_mode;
          entry.size = statx_buf_.stx_size;
          entry.is_directory = S_ISDIR(statx_buf_.stx_mode);
          entry.is_symlink = S_ISLNK(statx_buf_.stx_mode);
        }
        s.completed.push_back(entry);

        // Move to next entry
        s.current_index++;
        if (s.current_index >= s.entries.size()) {
          s.current_phase = state_type::phase::done;
        } else {
          ops.push_back(evring::operation::make_statx(
              AT_FDCWD, s.entries[s.current_index].path.c_str(), AT_SYMLINK_NOFOLLOW,
              STATX_TYPE | STATX_MODE | STATX_SIZE, evring::make_stable_ref(statx_buf_)));
        }
        break;
      }

      case state_type::phase::done:
      case state_type::phase::error:
        break;
    }

    return {std::move(s), std::move(ops)};
  }

  [[nodiscard]] auto done(const state_type& s) const -> bool {
    return s.current_phase == state_type::phase::done ||
           s.current_phase == state_type::phase::error;
  }

  std::vector<dir_entry> entries_;
  mutable struct statx statx_buf_; // Stable buffer for statx results
};

// ============================================================================
// Read directory entries (using opendir/readdir - synchronous but simple)
// ============================================================================

auto read_directory(const std::string& path) -> std::vector<dir_entry> {
  std::vector<dir_entry> entries;

  DIR* dir = opendir(path.c_str());
  if (!dir) {
    return entries;
  }

  struct dirent* ent;
  while ((ent = readdir(dir)) != nullptr) {
    // Skip . and ..
    if (std::strcmp(ent->d_name, ".") == 0 || std::strcmp(ent->d_name, "..") == 0) {
      continue;
    }

    dir_entry entry;
    entry.name = ent->d_name;
    entry.path = path + "/" + ent->d_name;
    entries.push_back(entry);
  }

  closedir(dir);
  return entries;
}

// ============================================================================
// Print tree
// ============================================================================

void print_tree(evring::ring& ring, const std::string& path, const std::string& prefix = "",
                int depth = 0, int max_depth = 10) {
  if (depth > max_depth) {
    std::printf("%s...\n", prefix.c_str());
    return;
  }

  // Read directory entries
  auto entries = read_directory(path);
  if (entries.empty()) {
    return;
  }

  // Stat all entries using evring
  stat_entries_machine statter{entries};
  auto state = evring::run(statter, ring);

  // Sort: directories first, then alphabetically
  auto& completed = state.completed;
  std::sort(completed.begin(), completed.end(), [](const dir_entry& a, const dir_entry& b) {
    if (a.is_directory != b.is_directory) {
      return a.is_directory > b.is_directory;
    }
    return a.name < b.name;
  });

  // Print entries
  for (std::size_t i = 0; i < completed.size(); ++i) {
    const auto& entry = completed[i];
    bool is_last = (i == completed.size() - 1);

    // Tree connector
    std::string connector = is_last ? "\xe2\x94\x94\xe2\x94\x80\xe2\x94\x80 "  // └──
                                    : "\xe2\x94\x9c\xe2\x94\x80\xe2\x94\x80 "; // ├──

    // Format entry
    std::string mode_str = format_mode(entry.mode);
    std::string size_str = entry.is_directory ? "<DIR>" : format_size(entry.size);

    std::printf("%s%s%s  %8s  %s", prefix.c_str(), connector.c_str(), mode_str.c_str(),
                size_str.c_str(), entry.name.c_str());

    if (entry.is_symlink) {
      std::printf(" -> ...");
    }
    std::printf("\n");

    // Recurse into directories
    if (entry.is_directory && !entry.is_symlink) {
      std::string new_prefix = prefix + (is_last ? "    " : "\xe2\x94\x82   "); // │ or space
      print_tree(ring, entry.path, new_prefix, depth + 1, max_depth);
    }
  }
}

// ============================================================================
// Main
// ============================================================================

int main(int argc, char* argv[]) {
  const char* path = argc > 1 ? argv[1] : ".";
  int max_depth = 10;

  // Parse optional max depth
  if (argc > 2) {
    max_depth = std::atoi(argv[2]);
    if (max_depth <= 0) {
      max_depth = 10;
    }
  }

  // Create ring
  auto ring = evring::make_io_uring_ring(64);
  if (!ring) {
    std::fprintf(stderr, "Failed to create io_uring\n");
    return 1;
  }

  // Print header
  std::printf("%s\n", path);

  // Print tree
  print_tree(*ring, path, "", 0, max_depth);

  return 0;
}
