#pragma once
///@file

#include <chrono>
#include <filesystem>
#include <optional>

#include "nix/util/types.h"

namespace nix {

std::optional<Path> get_cgroup_fs();

string_map_t get_cgroups(const Path& cgroup_file);

struct cgroup_stats_t {
  std::optional<std::chrono::microseconds> cpu_user, cpu_system;
};

/**
 * Read statistics from the given cgroup.
 */
cgroup_stats_t get_cgroup_stats(const std::filesystem::path& cgroup);

/**
 * Destroy the cgroup denoted by 'path'. The postcondition is that
 * 'path' does not exist, and thus any processes in the cgroup have
 * been killed. Also return statistics from the cgroup just before
 * destruction.
 */
cgroup_stats_t destroy_cgroup(const Path& cgroup);

std::string get_current_cgroup();

/**
 * Get the cgroup that should be used as the parent when creating new
 * sub-cgroups. The first time this is called, the current cgroup will be
 * returned, and then all subsequent calls will return the original cgroup.
 */
std::string get_root_cgroup();

/**
 * Get the PIDs of all processes in the given cgroup.
 */
std::set<::pid_t> get_pids_in_cgroup(const std::filesystem::path& cgroup);

} // namespace nix
