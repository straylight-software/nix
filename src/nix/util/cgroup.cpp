#include "nix/util/cgroup.h"

#include <chrono>
#include <cmath>
#include <regex>
#include <thread>

#include <dirent.h>
#include <mntent.h>

#include <boost/unordered/unordered_flat_set.hpp>

#include "nix/util/file-system.h"
#include "nix/util/finally.h"
#include "nix/util/signals.h"
#include "nix/util/util.h"

namespace nix {

std::optional<Path> get_cgroup_fs() {
  static auto res = [&]() -> std::optional<Path> {
    auto fp = fopen("/proc/mounts", "r");
    if (!fp)
      return std::nullopt;
    finally_t del_fp = [&]() { fclose(fp); };
    while (auto ent = getmntent(fp))
      if (std::string_view(ent->mnt_type) == "cgroup2")
        return ent->mnt_dir;

    return std::nullopt;
  }();
  return res;
}

// FIXME: obsolete, check for cgroup2
string_map_t get_cgroups(const Path& cgroup_file) {
  string_map_t cgroups;

  for (auto& line : tokenize_string<std::vector<std::string>>(read_file(cgroup_file), "\n")) {
    static std::regex regex("([0-9]+):([^:]*):(.*)");
    std::smatch match;
    if (!std::regex_match(line, match, regex))
      throw Error("invalid line '%s' in '%s'", line, cgroup_file);

    std::string name =
        has_prefix(std::string(match[2]), "name=") ? std::string(match[2], 5) : match[2];
    cgroups.insert_or_assign(name, match[3]);
  }

  return cgroups;
}

cgroup_stats_t get_cgroup_stats(const std::filesystem::path& cgroup) {
  cgroup_stats_t stats;

  auto cpustat_path = cgroup / "cpu.stat";

  if (path_exists(cpustat_path)) {
    for (auto& line : tokenize_string<std::vector<std::string>>(read_file(cpustat_path), "\n")) {
      std::string_view userPrefix = "user_usec ";
      if (has_prefix(line, userPrefix)) {
        auto n = string2_int<uint64_t>(line.substr(userPrefix.size()));
        if (n)
          stats.cpu_user = std::chrono::microseconds(*n);
      }

      std::string_view systemPrefix = "system_usec ";
      if (has_prefix(line, systemPrefix)) {
        auto n = string2_int<uint64_t>(line.substr(systemPrefix.size()));
        if (n)
          stats.cpu_system = std::chrono::microseconds(*n);
      }
    }
  }

  return stats;
}

static cgroup_stats_t destroy_cgroup(const std::filesystem::path& cgroup, bool return_stats) {
  if (!path_exists(cgroup))
    return {};

  auto procs_file = cgroup / "cgroup.procs";

  if (!path_exists(procs_file))
    throw Error("'%s' is not a cgroup", cgroup);

  /* use the fast way to kill every process in a cgroup, if
     available. */
  auto kill_file = cgroup / "cgroup.kill";
  if (path_exists(kill_file))
    write_file(kill_file, "1");

  /* Otherwise, manually kill every process in the subcgroups and
     this cgroup. */
  for (auto& entry : directory_iterator_t{cgroup}) {
    check_interrupt();
    if (entry.symlink_status().type() != std::filesystem::file_type::directory)
      continue;
    destroy_cgroup(cgroup / entry.path().filename(), false);
  }

  int round = 1;

  boost::unordered_flat_set<::pid_t> pids_shown;

  while (true) {
    auto pids = tokenize_string<std::vector<std::string>>(read_file(procs_file));

    if (pids.empty())
      break;

    if (round > 20)
      throw Error("cannot kill cgroup '%s'", cgroup);

    for (auto& pid_s : pids) {
      ::pid_t pid;
      if (auto o = string2_int<::pid_t>(pid_s))
        pid = *o;
      else
        throw Error("invalid pid '%s'", pid);
      if (pids_shown.insert(pid).second) {
        try {
          auto cmdline = read_file(fmt("/proc/%d/cmdline", pid));
          using namespace std::string_literals;
          warn("killing stray builder process %d (%s)...", pid,
               trim(replace_strings(cmdline, "\0"s, " ")));
        } catch (SystemError&) {
        }
      }
      // FIXME: pid wraparound
      if (kill(pid, SIGKILL) == -1 && errno != ESRCH)
        throw sys_error_t("killing member %d of cgroup '%s'", pid, cgroup);
    }

    auto sleep = std::chrono::milliseconds((int)std::pow(2.0, std::min(round, 10)));
    if (sleep.count() > 100)
      printError("waiting for %d ms for cgroup '%s' to become empty", sleep.count(), cgroup);
    std::this_thread::sleep_for(sleep);
    round++;
  }

  cgroup_stats_t stats;
  if (return_stats)
    stats = get_cgroup_stats(cgroup);

  if (rmdir(cgroup.c_str()) == -1)
    throw sys_error_t("deleting cgroup %s", cgroup);

  return stats;
}

cgroup_stats_t destroy_cgroup(const Path& cgroup) {
  return destroy_cgroup(cgroup, true);
}

std::string get_current_cgroup() {
  auto cgroup_fs = get_cgroup_fs();
  if (!cgroup_fs)
    throw Error("cannot determine the cgroups file system");

  auto our_cgroups = get_cgroups("/proc/self/cgroup");
  auto our_cgroup = our_cgroups[""];
  if (our_cgroup == "")
    throw Error("cannot determine cgroup name from /proc/self/cgroup");
  return our_cgroup;
}

std::string get_root_cgroup() {
  static std::string root_cgroup = get_current_cgroup();
  return root_cgroup;
}

std::set<::pid_t> get_pids_in_cgroup(const std::filesystem::path& cgroup) {
  if (!path_exists(cgroup))
    return {};

  auto procs_file = cgroup / "cgroup.procs";

  std::set<::pid_t> result;

  for (auto& pidStr : tokenize_string<std::vector<std::string>>(read_file(procs_file))) {
    if (auto o = string2_int<::pid_t>(pidStr))
      result.insert(*o);
    else
      throw Error("invalid PID '%s'", pidStr);
  }

  return result;
}

} // namespace nix
