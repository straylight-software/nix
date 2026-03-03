/**
 * Legacy nix-channel compatibility shim
 *
 * This provides nix-channel compatibility for scripts and tools that depend on it.
 *
 * nix-channel --update can hang indefinitely on network operations. This implementation
 * addresses issue #3236 by:
 * 1. Using FileTransfer with proper timeout settings (connect-timeout, stalled-download-timeout)
 * 2. Adding check_interrupt() calls in loops to respond to Ctrl-C
 * 3. Proper EINTR handling through the FileTransfer infrastructure
 *
 * For new projects, consider using flakes instead: https://nixos.wiki/wiki/Flakes
 */

#include <ctime>
#include <fstream>
#include <iostream>

#include <fcntl.h>

#include "nix/cmd/legacy.h"
#include "nix/store/filetransfer.h"
#include "nix/store/globals.h"
#include "nix/store/local-fs-store.h"
#include "nix/store/profiles.h"
#include "nix/store/store-open.h"
#include "nix/util/file-system.h"
#include "nix/util/finally.h"
#include "nix/util/signals.h"
#include "nix/util/source-accessor.h"
#include "nix/util/tarfile.h"
#include "nix/util/users.h"

namespace nix {

/**
 * Path to the channels file for the current user.
 * Uses XDG base directories if configured.
 */
static std::filesystem::path get_channels_file() {
  if (settings.useXDGBaseDirectories) {
    return std::filesystem::path{create_nix_state_dir()} / "channels";
  } else {
    return std::filesystem::path{get_home()} / ".nix-channels";
  }
}

/**
 * Channel entry: name -> URL mapping
 */
struct Channel {
  std::string name;
  std::string url;
};

/**
 * Derive a channel name from a URL.
 */
static std::string derive_name_from_url(const std::string& url) {
  // Try to extract name from path
  auto slash_pos = url.rfind('/');
  if (slash_pos != std::string::npos && slash_pos + 1 < url.size()) {
    auto candidate = url.substr(slash_pos + 1);
    // Skip common tarball names
    if (candidate != "nixexprs.tar.xz" && candidate != "nixexprs.tar.bz2" && !candidate.empty()) {
      // Strip any query string
      auto query_pos = candidate.find('?');
      if (query_pos != std::string::npos) {
        candidate = candidate.substr(0, query_pos);
      }
      // Strip any extension
      if (has_suffix(candidate, ".tar.xz") || has_suffix(candidate, ".tar.bz2") ||
          has_suffix(candidate, ".tar.gz")) {
        return candidate.substr(0, candidate.rfind(".tar"));
      }
      return candidate;
    }
    // Try parent path segment
    auto parent_end = slash_pos;
    if (parent_end > 0) {
      auto parent_start = url.rfind('/', parent_end - 1);
      if (parent_start != std::string::npos) {
        return url.substr(parent_start + 1, parent_end - parent_start - 1);
      }
    }
  }
  return "unknown";
}

/**
 * Read channels from the channels file.
 * Format: URL [name]\n per line
 */
static std::vector<Channel> read_channels() {
  std::vector<Channel> channels;
  auto path = get_channels_file();

  if (!path_exists(path)) {
    return channels;
  }

  std::ifstream file(path);
  std::string line;
  while (std::getline(file, line)) {
    check_interrupt(); // Issue #3236: Allow interruption during file reading

    // Skip empty lines and comments
    auto trimmed = trim(line);
    if (trimmed.empty() || trimmed[0] == '#') {
      continue;
    }

    // Parse "URL [name]" format
    auto space_pos = trimmed.find(' ');
    if (space_pos == std::string::npos) {
      // URL only, derive name from URL
      auto url = std::string(trimmed);
      auto name = derive_name_from_url(url);
      channels.push_back({.name = name, .url = url});
    } else {
      auto url = std::string(trimmed.substr(0, space_pos));
      auto name = std::string(trimmed.substr(space_pos + 1));
      channels.push_back({.name = trim(name), .url = url});
    }
  }

  return channels;
}

/**
 * Write channels to the channels file.
 */
static void write_channels(const std::vector<Channel>& channels) {
  auto path = get_channels_file();
  create_dirs(path.parent_path());

  std::ofstream file(path);
  for (const auto& channel : channels) {
    file << channel.url << " " << channel.name << "\n";
  }
}

/**
 * Add a channel.
 */
static void cmd_add(const std::string& url, const std::string& name) {
  auto channels = read_channels();

  // Check if channel with this name already exists
  for (auto& channel : channels) {
    if (channel.name == name) {
      channel.url = url;
      write_channels(channels);
      return;
    }
  }

  channels.push_back({.name = name, .url = url});
  write_channels(channels);
}

/**
 * Remove a channel by name.
 */
static void cmd_remove(const std::string& name) {
  auto channels = read_channels();
  bool found = false;

  std::vector<Channel> remaining;
  for (const auto& channel : channels) {
    if (channel.name == name) {
      found = true;
    } else {
      remaining.push_back(channel);
    }
  }

  if (!found) {
    throw Error("channel '%s' does not exist", name);
  }

  write_channels(remaining);
}

/**
 * List all channels.
 */
static void cmd_list() {
  auto channels = read_channels();
  for (const auto& channel : channels) {
    check_interrupt(); // Issue #3236: Allow interruption
    std::cout << channel.url << " " << channel.name << "\n";
  }
}

/**
 * Download and unpack a channel tarball.
 * Uses FileTransfer with proper timeout handling for issue #3236.
 */
static store_path_t fetch_channel(ref<store_t> store, const Channel& channel) {
  // Construct URL to channel tarball
  auto url = channel.url;
  if (!has_suffix(url, "/nixexprs.tar.xz") && !has_suffix(url, "/nixexprs.tar.bz2") &&
      !has_suffix(url, ".tar.xz") && !has_suffix(url, ".tar.bz2") && !has_suffix(url, ".tar.gz")) {
    // Append the standard tarball name if not already a tarball URL
    if (!has_suffix(url, "/")) {
      url += "/";
    }
    url += "nixexprs.tar.xz";
  }

  printInfo("fetching channel '%s' from '%s'", channel.name, url);

  // Issue #3236: FileTransfer already has proper timeout handling via:
  // - file_transfer_settings.connectTimeout (default 15s)
  // - file_transfer_settings.stalledDownloadTimeout (default 300s)
  // The check_interrupt() is called in the progress callback.
  auto_delete_t tmp_dir(create_temp_dir(), true);
  auto tmp_file = tmp_dir.path() / "channel.tar";

  {
    FileTransferRequest req(url);
    req.decompress = false;

    auto_close_fd_t fd =
        to_descriptor(open(tmp_file.string().c_str(), O_WRONLY | O_CREAT | O_EXCL, 0600));
    if (!fd) {
      throw sys_error_t("creating temporary file '%s'", tmp_file);
    }

    fd_sink_t sink(fd.get());
    get_file_transfer()->download(std::move(req), sink);
  }

  check_interrupt(); // Issue #3236: Check after download

  // Unpack the tarball
  printInfo("unpacking channel '%s'", channel.name);
  auto unpacked_dir = tmp_dir.path() / "unpacked";
  create_dirs(unpacked_dir);
  unpack_tarfile(tmp_file.string(), unpacked_dir.string());

  check_interrupt(); // Issue #3236: Check after unpack

  // Find the single top-level directory in the unpacked tarball
  std::filesystem::path source_dir;
  int entry_count = 0;
  for (const auto& entry : directory_iterator_t{unpacked_dir}) {
    check_interrupt(); // Issue #3236: Allow interruption during directory scan
    source_dir = entry.path();
    entry_count++;
  }

  if (entry_count != 1) {
    throw Error("channel tarball '%s' does not contain exactly one directory", url);
  }

  // Add to the store using proper source accessor
  auto accessor = make_fs_source_accessor(source_dir);
  source_path_t source_path{accessor, canon_path_t::root};
  auto info =
      store->addToStoreSlow(channel.name, source_path, content_address_method_t::raw_t::nix_archive,
                            hash_algorithm_t::SHA256);

  return info.path;
}

/**
 * Update channels.
 * Addresses issue #3236 by using proper timeout handling and check_interrupt().
 */
static void cmd_update(ref<store_t> store, const std::vector<std::string>& names) {
  auto channels = read_channels();

  if (channels.empty()) {
    printInfo("no channels to update");
    return;
  }

  // Filter channels if specific names requested
  std::vector<Channel> to_update;
  if (names.empty()) {
    to_update = channels;
  } else {
    for (const auto& name : names) {
      check_interrupt(); // Issue #3236: Allow interruption
      bool found = false;
      for (const auto& channel : channels) {
        if (channel.name == name) {
          to_update.push_back(channel);
          found = true;
          break;
        }
      }
      if (!found) {
        throw Error("channel '%s' does not exist", name);
      }
    }
  }

  // Create a combined expression that imports all channels
  std::string expr = "{ ";

  for (const auto& channel : to_update) {
    check_interrupt(); // Issue #3236: Allow interruption during loop

    printInfo("updating channel '%s'", channel.name);
    auto store_path = fetch_channel(store, channel);

    // Add to expression
    expr += channel.name + " = import " + store->printStorePath(store_path) + "; ";
  }

  expr += "}";

  check_interrupt(); // Issue #3236: Check before profile update

  // Write the combined expression to a file and add to store
  auto_delete_t tmp_dir(create_temp_dir(), true);
  auto expr_file = tmp_dir.path() / "default.nix";
  write_file(expr_file, expr);

  auto accessor = make_fs_source_accessor(tmp_dir.path());
  source_path_t source_path{accessor, canon_path_t::root};
  auto expr_info =
      store->addToStoreSlow("channels", source_path, content_address_method_t::raw_t::nix_archive,
                            hash_algorithm_t::SHA256);

  // Update the channels profile
  auto profile = default_channels_dir();
  create_dirs(profile.parent_path());

  // Create a new generation
  auto local_store = store.dynamic_pointer_cast<local_fs_store>();
  if (local_store) {
    PathLocks lock;
    lock_profile(lock, profile);
    auto gen_path = create_generation(*local_store, profile, expr_info.path);
    switch_link(profile, gen_path);
  }

  printInfo("channel update complete");
}

/**
 * Rollback channels to previous generation.
 */
static void cmd_rollback(const std::optional<GenerationNumber>& gen) {
  auto profile = default_channels_dir();
  switch_generation(profile, gen, false);
}

/**
 * List channel generations.
 */
static void cmd_list_generations() {
  auto profile = default_channels_dir();
  if (!path_exists(profile.parent_path())) {
    return;
  }

  auto [gens, current_gen] = findGenerations(profile);

  for (const auto& gen : gens) {
    check_interrupt(); // Issue #3236: Allow interruption

    std::cout << gen.number << " ";

    char time_buf[64];
    struct tm* tm_info = localtime(&gen.creationTime);
    strftime(time_buf, sizeof(time_buf), "%Y-%m-%d %H:%M:%S", tm_info);
    std::cout << time_buf;

    if (current_gen && *current_gen == gen.number) {
      std::cout << " (current)";
    }

    std::cout << "\n";
  }
}

static void main_nix_channel(int argc, char** argv) {
  enum class Command { None, Add, Remove, List, Update, Rollback, ListGenerations };

  Command cmd = Command::None;
  std::string url;
  std::string name;
  std::vector<std::string> update_names;
  std::optional<GenerationNumber> rollback_gen;
  bool show_help = false;

  // Parse arguments
  for (int i = 1; i < argc; i++) {
    check_interrupt(); // Issue #3236: Allow interruption during arg parsing

    std::string arg = argv[i];

    if (arg == "--help" || arg == "-h" || arg == "-?") {
      show_help = true;
    } else if (arg == "--add") {
      cmd = Command::Add;
      if (i + 1 < argc) {
        url = argv[++i];
        if (i + 1 < argc && argv[i + 1][0] != '-') {
          name = argv[++i];
        } else {
          // Derive name from URL
          name = derive_name_from_url(url);
        }
      } else {
        throw UsageError("--add requires a URL argument");
      }
    } else if (arg == "--remove") {
      cmd = Command::Remove;
      if (i + 1 < argc) {
        name = argv[++i];
      } else {
        throw UsageError("--remove requires a channel name argument");
      }
    } else if (arg == "--list") {
      cmd = Command::List;
    } else if (arg == "--update") {
      cmd = Command::Update;
      // Collect optional channel names to update
      while (i + 1 < argc && argv[i + 1][0] != '-') {
        update_names.push_back(argv[++i]);
      }
    } else if (arg == "--rollback") {
      cmd = Command::Rollback;
      if (i + 1 < argc && argv[i + 1][0] != '-') {
        rollback_gen = string2_int<GenerationNumber>(argv[++i]);
      }
    } else if (arg == "--list-generations") {
      cmd = Command::ListGenerations;
    } else if (arg == "--version") {
      std::cout << "nix-channel (Nix) " << nix_version << "\n";
      return;
    } else if (arg[0] == '-') {
      throw UsageError("unrecognized option: %s", arg);
    } else {
      // Positional argument - could be URL or channel name depending on command
      if (cmd == Command::Add && url.empty()) {
        url = arg;
      } else if (cmd == Command::Add && name.empty()) {
        name = arg;
      } else if (cmd == Command::Remove && name.empty()) {
        name = arg;
      } else if (cmd == Command::Update) {
        update_names.push_back(arg);
      } else {
        throw UsageError("unexpected argument: %s", arg);
      }
    }
  }

  if (show_help || cmd == Command::None) {
    std::cout << R"(nix-channel - manage Nix channels

Usage:
  nix-channel --add <url> [<name>]    Add or update a channel
  nix-channel --remove <name>         Remove a channel
  nix-channel --list                  List subscribed channels
  nix-channel --update [<names>...]   Update channels (all if none specified)
  nix-channel --rollback [<gen>]      Roll back to previous (or specific) generation
  nix-channel --list-generations      List channel generations
  nix-channel --version               Show version
  nix-channel --help                  Show this help

Examples:
  nix-channel --add https://nixos.org/channels/nixos-unstable nixos
  nix-channel --update
  nix-channel --update nixos
  nix-channel --rollback

Note: For new projects, consider using Nix flakes instead.
See: https://nixos.wiki/wiki/Flakes
)";
    return;
  }

  switch (cmd) {
    case Command::Add:
      cmd_add(url, name);
      break;
    case Command::Remove:
      cmd_remove(name);
      break;
    case Command::List:
      cmd_list();
      break;
    case Command::Update: {
      auto store = open_store();
      cmd_update(store, update_names);
      break;
    }
    case Command::Rollback:
      cmd_rollback(rollback_gen);
      break;
    case Command::ListGenerations:
      cmd_list_generations();
      break;
    case Command::None:
      // Already handled above with help
      break;
  }
}

static RegisterLegacyCommand r_nix_channel("nix-channel", main_nix_channel);

} // namespace nix
