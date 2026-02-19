#include <fcntl.h>

#include "nix/store/local-store.h"
#include "nix/util/json-utils.h"
#ifdef __linux__
#  include <regex>

#  include <pwd.h>
#  include <unistd.h>

#  include "nix/util/cgroup.h"
#endif

#ifdef __APPLE__
#  include <libproc.h>
#  include <mach/mach_time.h>
#  include <sys/sysctl.h>
#endif

#include <queue>

#include <nlohmann/json.hpp>

namespace nix {

#ifdef __linux__
static ActiveBuildInfo::ProcessInfo get_process_info(pid_t pid) {
  ActiveBuildInfo::ProcessInfo info;
  info.pid = pid;
  info.argv = tokenize_string<std::vector<std::string>>(read_file(fmt("/proc/%d/cmdline", pid)),
                                                        std::string("\000", 1));

  auto stat_path = fmt("/proc/%d/stat", pid);

  auto_close_fd_t stat_fd = open(stat_path.c_str(), O_RDONLY | O_CLOEXEC);
  if (!stat_fd)
    throw sys_error_t("opening '%s'", stat_path);

  // Get the UID from the ownership of the stat file.
  struct stat st;
  if (fstat(stat_fd.get(), &st) == -1)
    throw sys_error_t("getting ownership of '%s'", stat_path);
  info.user = UserInfo::fromUid(st.st_uid);

  // Read /proc/[pid]/stat for parent PID and CPU times.
  // Format: pid (comm) state ppid ...
  // Note that the comm field can contain spaces, so use a regex to parse it.
  auto stat_content = trim(read_file(stat_fd.get()));
  static std::regex stat_regex(R"((\d+) \(([^)]*)\) (.*))");
  std::smatch match;
  if (!std::regex_match(stat_content, match, stat_regex))
    throw Error("failed to parse /proc/%d/stat", pid);

  // Parse the remaining fields after (comm).
  auto remaining_fields = tokenize_string<std::vector<std::string>>(match[3].str());

  if (remaining_fields.size() > 1)
    info.parentPid = string2_int<pid_t>(remaining_fields[1]).value_or(0);

  static long clk_tck = sysconf(_SC_CLK_TCK);
  if (remaining_fields.size() > 14 && clk_tck > 0) {
    if (auto utime = string2_int<uint64_t>(remaining_fields[11]))
      info.utime = std::chrono::microseconds((*utime * 1'000'000) / clk_tck);
    if (auto stime = string2_int<uint64_t>(remaining_fields[12]))
      info.stime = std::chrono::microseconds((*stime * 1'000'000) / clk_tck);
    if (auto cutime = string2_int<uint64_t>(remaining_fields[13]))
      info.cutime = std::chrono::microseconds((*cutime * 1'000'000) / clk_tck);
    if (auto cstime = string2_int<uint64_t>(remaining_fields[14]))
      info.cstime = std::chrono::microseconds((*cstime * 1'000'000) / clk_tck);
  }

  return info;
}

/**
 * Recursively get all descendant PIDs of a given PID using /proc/[pid]/task/[pid]/children.
 */
static std::set<pid_t> get_descendant_pids(pid_t pid) {
  std::set<pid_t> descendants;

  [&](this auto self, pid_t pid) -> void {
    try {
      descendants.insert(pid);
      for (const auto& childPidStr : tokenize_string<std::vector<std::string>>(
               read_file(fmt("/proc/%d/task/%d/children", pid, pid))))
        if (auto childPid = string2_int<pid_t>(childPidStr))
          self(*childPid);
    } catch (...) {
      // Process may have exited.
      ignore_exception_except_interrupt();
    }
  }(pid);

  return descendants;
}
#endif

#ifdef __APPLE__
static ActiveBuildInfo::ProcessInfo get_process_info(pid_t pid) {
  ActiveBuildInfo::ProcessInfo info;
  info.pid = pid;

  // Get basic process info including ppid and uid.
  struct proc_bsdinfo procInfo;
  if (proc_pidinfo(pid, PROC_PIDTBSDINFO, 0, &procInfo, sizeof(procInfo)) != sizeof(procInfo))
    throw sys_error_t("getting process info for pid %d", pid);

  info.parentPid = procInfo.pbi_ppid;
  info.user = UserInfo::fromUid(procInfo.pbi_uid);

  // Get CPU times.
  struct proc_taskinfo taskInfo;
  if (proc_pidinfo(pid, PROC_PIDTASKINFO, 0, &taskInfo, sizeof(taskInfo)) == sizeof(taskInfo)) {
    mach_timebase_info_data_t timebase;
    mach_timebase_info(&timebase);
    auto nanosecondsPerTick = (double)timebase.numer / (double)timebase.denom;

    // Convert nanoseconds to microseconds.
    info.utime = std::chrono::microseconds(
        (uint64_t)((double)taskInfo.pti_total_user * nanosecondsPerTick / 1000));
    info.stime = std::chrono::microseconds(
        (uint64_t)((double)taskInfo.pti_total_system * nanosecondsPerTick / 1000));
  }

  // Get argv using sysctl.
  int mib[3] = {CTL_KERN, KERN_PROCARGS2, pid};
  size_t size = 0;

  // First call to get size.
  if (sysctl(mib, 3, nullptr, &size, nullptr, 0) == 0 && size > 0) {
    std::vector<char> buffer(size);
    if (sysctl(mib, 3, buffer.data(), &size, nullptr, 0) == 0) {
      // Format: argc (int), followed by executable path, followed by null-terminated args
      if (size >= sizeof(int)) {
        int argc;
        memcpy(&argc, buffer.data(), sizeof(argc));

        // Skip past argc and executable path (null-terminated).
        size_t pos = sizeof(int);
        while (pos < size && buffer[pos] != '\0')
          pos++;
        pos++; // Skip the null terminator

        // Parse the arguments.
        while (pos < size && info.argv.size() < (size_t)argc) {
          size_t argStart = pos;
          while (pos < size && buffer[pos] != '\0')
            pos++;

          if (pos > argStart)
            info.argv.emplace_back(buffer.data() + argStart, pos - argStart);

          pos++; // Skip the null terminator
        }
      }
    }
  }

  return info;
}

/**
 * Recursively get all descendant PIDs using sysctl with KERN_PROC.
 */
static std::set<pid_t> get_descendant_pids(pid_t startPid) {
  // Get all processes.
  int mib[4] = {CTL_KERN, KERN_PROC, KERN_PROC_ALL, 0};
  size_t size = 0;

  if (sysctl(mib, 4, nullptr, &size, nullptr, 0) == -1)
    return {startPid};

  std::vector<struct kinfo_proc> procs(size / sizeof(struct kinfo_proc));
  if (sysctl(mib, 4, procs.data(), &size, nullptr, 0) == -1)
    return {startPid};

  // Get the children of all processes.
  std::map<pid_t, std::set<pid_t>> children;
  size_t count = size / sizeof(struct kinfo_proc);
  for (size_t i = 0; i < count; i++) {
    pid_t childPid = procs[i].kp_proc.p_pid;
    pid_t parentPid = procs[i].kp_eproc.e_ppid;
    children[parentPid].insert(childPid);
  }

  // Get all children of `pid`.
  std::set<pid_t> descendants;
  std::queue<pid_t> todo;
  todo.push(startPid);
  while (auto pid = pop(todo)) {
    if (!descendants.insert(*pid).second)
      continue;
    for (auto& child : children[*pid])
      todo.push(child);
  }

  return descendants;
}
#endif

std::vector<ActiveBuildInfo> LocalStore::queryActiveBuilds() {
  std::vector<ActiveBuildInfo> result;

  for (auto& entry : directory_iterator_t{activeBuildsDir}) {
    auto path = entry.path();

    try {
      // Open the file. If we can lock it, the build is not active.
      auto fd = open_lock_file(path, false);
      if (!fd || lock_file(fd.get(), ltRead, false)) {
        auto_delete_t(path, false);
        continue;
      }

      ActiveBuildInfo info(nlohmann::json::parse(read_file(fd.get())).get<ActiveBuild>());

#if defined(__linux__) || defined(__APPLE__)
      /* Read process information. */
      try {
#  ifdef __linux__
        if (info.cgroup) {
          for (auto pid : get_pids_in_cgroup(*info.cgroup))
            info.processes.push_back(get_process_info(pid));

          /* Read CPU statistics from the cgroup. */
          auto stats = get_cgroup_stats(*info.cgroup);
          info.utime = stats.cpu_user;
          info.stime = stats.cpu_system;
        } else
#  endif
        {
          for (auto pid : get_descendant_pids(info.mainPid))
            info.processes.push_back(get_process_info(pid));
        }
      } catch (...) {
        ignore_exception_except_interrupt();
      }
#endif

      result.push_back(std::move(info));
    } catch (...) {
      ignore_exception_except_interrupt();
    }
  }

  return result;
}

LocalStore::BuildHandle LocalStore::buildStarted(const ActiveBuild& build) {
  // Write info about the active build to the active-builds directory where it can be read by
  // `queryBuilds()`.
  static std::atomic<uint64_t> next_id{1};

  auto id = next_id++;

  auto infoFileName = fmt("%d-%d", getpid(), id);
  auto infoFilePath = activeBuildsDir / infoFileName;

  auto infoFd = open_lock_file(infoFilePath, true);

  // Lock the file to denote that the build is active.
  lock_file(infoFd.get(), ltWrite, true);

  write_file(infoFilePath, nlohmann::json(build).dump(), 0600, fs_sync_t::yes);

  active_builds.lock()->emplace(id, ActiveBuildFile{
                                        .fd = std::move(infoFd),
                                        .del = auto_delete_t(infoFilePath, false),
                                    });

  return BuildHandle(*this, id);
}

void LocalStore::buildFinished(const BuildHandle& handle) {
  active_builds.lock()->erase(handle.id);
}

} // namespace nix
