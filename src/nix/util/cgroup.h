#pragma once
///@file

#include <chrono>
#include <filesystem>
#include <optional>

#include "nix/util/types.h"

namespace nix {

std::optional<Path> getCgroupFS();

string_map_t getCgroups(const Path& cgroupFile);

struct cgroup_stats_t {
  std::optional<std::chrono::microseconds> cpuUser, cpuSystem;
};

/**
 * Read statistics from the given cgroup.
 */
cgroup_stats_t getCgroupStats(const std::filesystem::path& cgroup);

/**
 * Destroy the cgroup denoted by 'path'. The postcondition is that
 * 'path' does not exist, and thus any processes in the cgroup have
 * been killed. Also return statistics from the cgroup just before
 * destruction.
 */
cgroup_stats_t destroyCgroup(const Path& cgroup);

std::string getCurrentCgroup();

/**
 * Get the cgroup that should be used as the parent when creating new
 * sub-cgroups. The first time this is called, the current cgroup will be
 * returned, and then all subsequent calls will return the original cgroup.
 */
std::string getRootCgroup();

/**
 * Get the PIDs of all processes in the given cgroup.
 */
std::set<pid_t> getPidsInCgroup(const std::filesystem::path& cgroup);

} // namespace nix
