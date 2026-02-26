#include <nlohmann/json.hpp>

#include "nix/cmd/command.h"
#include "nix/main/common-args.h"
#include "nix/main/shared.h"
#include "nix/store/active-builds.h"
#include "nix/store/store-api.h"
#include "nix/store/store-cast.h"
#include "nix/util/table.h"
#include "nix/util/terminal.h"

// Required for notice macro
using nix::fmt;
using nix::logger;

struct cmd_ps_t : nix::MixJSON, nix::StoreCommand {
  std::string description() override { return "list active builds"; }

  category_t category() override { return nix::catUtility; }

  std::string doc() override {
    return
#include "ps.md"
        ;
  }

  void run(nix::ref<nix::store_t> store) override {
    auto& tracker = nix::require<nix::QueryActiveBuildsStore>(*store);

    auto builds = tracker.queryActiveBuilds();

    if (json) {
      printJSON(nlohmann::json(builds));
      return;
    }

    if (builds.empty()) {
      notice("No active builds.");
      return;
    }

    /* Helper to format user info: show name if available, else UID */
    auto format_user = [](const nix::UserInfo& user) -> std::string {
      return user.name ? *user.name : std::to_string(user.uid);
    };

    nix::table_t table;

    /* Add column headers. */
    table.push_back({{"USER"},
                     {"PID"},
                     {"CPU", nix::table_cell_t::alignment_t::right},
                     {"DERIVATION/COMMAND"}});

    for (const auto& build : builds) {
      /* Calculate CPU time - use cgroup stats if available, otherwise sum process times. */
      std::chrono::microseconds cpuTime =
          build.utime && build.stime ? *build.utime + *build.stime : [&]() {
            std::chrono::microseconds total{0};
            for (const auto& process : build.processes)
              total += process.utime.value_or(std::chrono::microseconds(0)) +
                       process.stime.value_or(std::chrono::microseconds(0)) +
                       process.cutime.value_or(std::chrono::microseconds(0)) +
                       process.cstime.value_or(std::chrono::microseconds(0));
            return total;
          }();

      /* Add build summary row. */
      table.push_back(
          {format_user(build.mainUser),
           std::to_string(build.main_pid),
           {nix::fmt("%.1fs",
                     std::chrono::duration_cast<
                         std::chrono::duration<float, std::chrono::seconds::period>>(cpuTime)
                         .count()),
            nix::table_cell_t::alignment_t::right},
           nix::fmt(ANSI_BOLD "%s" ANSI_NORMAL " (wall=%ds)",
                    store->printStorePath(build.derivation), time(nullptr) - build.start_time)});

      if (build.processes.empty()) {
        table.push_back(
            {format_user(build.mainUser),
             std::to_string(build.main_pid),
             {"", nix::table_cell_t::alignment_t::right},
             nix::fmt("%s" ANSI_ITALIC "(no process info)" ANSI_NORMAL, nix::tree_last)});
      } else {
        /* Recover the tree structure of the processes. */
        std::set<::pid_t> pids;
        for (auto& process : build.processes)
          pids.insert(process.pid);

        using Processes = std::set<const nix::ActiveBuildInfo::ProcessInfo*>;
        std::map<::pid_t, Processes> children;
        Processes rootProcesses;
        for (auto& process : build.processes) {
          if (pids.contains(process.parent_pid))
            children[process.parent_pid].insert(&process);
          else
            rootProcesses.insert(&process);
        }

        /* Render the process tree. */
        [&](this auto const& visit, const Processes& processes, std::string_view prefix) -> void {
          for (const auto& [n, process] : nix::enumerate(processes)) {
            bool last = n + 1 == processes.size();

            // Format CPU time if available
            std::string cpuInfo;
            if (process->utime || process->stime || process->cutime || process->cstime) {
              auto totalCpu = process->utime.value_or(std::chrono::microseconds(0)) +
                              process->stime.value_or(std::chrono::microseconds(0)) +
                              process->cutime.value_or(std::chrono::microseconds(0)) +
                              process->cstime.value_or(std::chrono::microseconds(0));
              auto totalSecs =
                  std::chrono::duration_cast<
                      std::chrono::duration<float, std::chrono::seconds::period>>(totalCpu)
                      .count();
              cpuInfo = nix::fmt("%.1fs", totalSecs);
            }

            // Format argv with tree structure
            auto argv =
                nix::concat_strings_sep(" ", nix::tokenize_string<std::vector<std::string>>(
                                                 nix::concat_strings_sep(" ", process->argv)));

            table.push_back(
                {format_user(process->user),
                 std::to_string(process->pid),
                 {cpuInfo, nix::table_cell_t::alignment_t::right},
                 nix::fmt("%s%s%s", prefix, last ? nix::tree_last : nix::tree_conn, argv)});

            visit(children[process->pid], std::string(last ? prefix : prefix) +
                                              std::string(last ? nix::tree_null : nix::tree_line));
          }
        }(rootProcesses, "");
      }
    }

    auto width = nix::is_tty() && isatty(STDOUT_FILENO) ? nix::get_window_width()
                                                        : std::numeric_limits<unsigned int>::max();

    nix::print_table(std::cout, table, width);
  }
};

static auto r_cmd_ps = nix::registerCommand2<cmd_ps_t>({"ps"});
