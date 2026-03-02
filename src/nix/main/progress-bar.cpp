#include "nix/main/progress-bar.h"

#include <atomic>
#include <chrono>
#include <iostream>
#include <map>
#include <thread>

#include "nix/store/names.h"
#include "nix/store/store-api.h"
#include "nix/util/sync.h"
#include "nix/util/terminal.h"

namespace nix {

static std::string_view get_s(const std::vector<logger_t::field_t>& fields, size_t n) {
  assert(n < fields.size());
  assert(fields[n].type_ == logger_t::field_t::t_string);
  return fields[n].s_;
}

static uint64_t get_i(const std::vector<logger_t::field_t>& fields, size_t n) {
  assert(n < fields.size());
  assert(fields[n].type_ == logger_t::field_t::t_int);
  return fields[n].i_;
}

static std::string_view store_path_to_name(std::string_view path) {
  auto base = base_name_of(path);
  auto i = base.find('-');
  return i == std::string::npos ? base.substr(0, 0) : base.substr(i + 1);
}

struct progress_bar_t : public logger_t {
  struct act_info_t {
    std::string s, last_line, phase;
    activity_type_t type = act_unknown;
    uint64_t done = 0;
    uint64_t expected = 0;
    uint64_t running = 0;
    uint64_t failed = 0;
    std::map<activity_type_t, uint64_t> expected_by_type;
    bool visible = true;
    activity_id_t parent;
    std::optional<std::string> name;
    std::chrono::time_point<std::chrono::steady_clock> start_time;
    bool logged = false;
  };

  struct activities_by_type_t {
    std::map<activity_id_t, std::list<act_info_t>::iterator> its;
    uint64_t done = 0;
    uint64_t expected = 0;
    uint64_t failed = 0;
  };

  struct State {
    std::list<act_info_t> activities;
    std::map<activity_id_t, std::list<act_info_t>::iterator> its;

    std::map<activity_type_t, activities_by_type_t> activities_by_type;

    uint64_t files_linked = 0, bytes_linked = 0;

    uint64_t corrupted_paths = 0, untrusted_paths = 0;

    bool active = true;
    size_t suspensions = 0;
    bool have_update = true;

    bool is_paused() const { return suspensions > 0; }
  };

  /** Helps avoid unnecessary redraws, see `redraw()` */
  sync_t<std::string> lastOutput_;

  sync_t<State> state_;

  std::thread updateThread;

  std::condition_variable quitCV, updateCV;

  bool print_build_logs = false;
  bool is_tty;

  progress_bar_t(bool is_tty) : is_tty(is_tty) {
    state_.lock()->active = is_tty;
    updateThread = std::thread([&]() {
      auto state(state_.lock());
      auto next_wakeup = std::chrono::milliseconds::max();
      while (state->active) {
        if (!state->have_update) {
          state.wait_for(updateCV, next_wakeup);
        }
        next_wakeup = draw(*state);
        state.wait_for(quitCV, std::chrono::milliseconds(50));
      }
    });
  }

  ~progress_bar_t() { stop(); }

  /* Called by destructor, can't be overridden */
  void stop() override final {
    {
      auto state(state_.lock());
      if (state->active) {
        state->active = false;
        write_to_stderr("\r\e[K");
        updateCV.notify_one();
        quitCV.notify_one();
      }
    }
    if (updateThread.joinable()) {
      updateThread.join();
    }
  }

  void pause() override {
    auto state(state_.lock());
    state->suspensions++;
    if (state->suspensions > 1) {
      // already paused
      return;
    }

    if (state->active) {
      write_to_stderr("\r\e[K");
      /* Show activities that were previously only shown on the
         progress bar. Otherwise the user won't know what's
         happening. */
      for (auto& act : state->activities) {
        log_activity(*state, lvl_notice, act);
      }
    }
  }

  void resume() override {
    auto state(state_.lock());
    if (state->suspensions == 0) {
      log(lvl_error,
          "nix::ProgressBar: resume() called without a matching preceding pause(). This is a bug.");
      return;
    } else {
      state->suspensions--;
    }
    if (state->suspensions == 0) {
      if (state->active) {
        write_to_stderr("\r\e[K");
      }
      state->have_update = true;
      updateCV.notify_one();
    }
  }

  bool is_verbose() override { return print_build_logs; }

  void log(verbosity_t lvl, std::string_view s) override {
    if (lvl > verbosity) {
      return;
    }
    auto state(state_.lock());
    log(*state, lvl, s);
  }

  void log_ei(const error_info_t& ei) override {
    auto state(state_.lock());
    log(*state, ei.level_, format_error_info(ei, logger_settings.show_trace.get()));
  }

  void log(State& state, verbosity_t lvl, std::string_view s) {
    if (state.active) {
      write_to_stderr("\r\e[K" + filter_ansi_escapes(s, !is_tty) + ANSI_NORMAL "\n");
      draw(state);
    } else {
      write_to_stderr(filter_ansi_escapes(s, !is_tty) + "\n");
    }
  }

  void log_activity(State& state, verbosity_t lvl, act_info_t& act) {
    if (!act.logged && lvl <= verbosity && !act.s.empty() && act.type != act_build_waiting) {
      log(state, lvl, act.s + "...");
      act.logged = true;
    }
  }

  void start_activity(activity_id_t act, verbosity_t lvl, activity_type_t type,
                      const std::string& s, const fields_t& fields, activity_id_t parent) override {
    auto state(state_.lock());

    state->activities.emplace_back(act_info_t{
        .s = s, .type = type, .parent = parent, .start_time = std::chrono::steady_clock::now()});
    auto i = std::prev(state->activities.end());
    state->its.emplace(act, i);
    state->activities_by_type[type].its.emplace(act, i);

    log_activity(*state, lvl, *i);

    if (type == act_build) {
      std::string name(store_path_to_name(get_s(fields, 0)));
      if (has_suffix(name, ".drv")) {
        name = name.substr(0, name.size() - 4);
      }
      i->s = fmt("building " ANSI_BOLD "%s" ANSI_NORMAL, name);
      auto machine_name = get_s(fields, 1);
      if (machine_name != "") {
        i->s += fmt(" on " ANSI_BOLD "%s" ANSI_NORMAL, machine_name);
      }

      // Used to be curRound and nrRounds, but the
      // implementation was broken for a long time.
      if (get_i(fields, 2) != 1 || get_i(fields, 3) != 1) {
        throw Error(
            "log message indicated repeating builds, but this is not currently implemented");
      }
      i->name = DrvName(name).name;
    }

    if (type == act_substitute) {
      auto name = store_path_to_name(get_s(fields, 0));
      auto sub = get_s(fields, 1);
      i->s = fmt(has_prefix(sub, "local") ? "copying " ANSI_BOLD "%s" ANSI_NORMAL " from %s"
                                          : "fetching " ANSI_BOLD "%s" ANSI_NORMAL " from %s",
                 name, sub);
    }

    if (type == act_post_build_hook) {
      auto name = store_path_to_name(get_s(fields, 0));
      if (has_suffix(name, ".drv")) {
        name = name.substr(0, name.size() - 4);
      }
      i->s = fmt("post-build " ANSI_BOLD "%s" ANSI_NORMAL, name);
      i->name = DrvName(name).name;
    }

    if (type == act_query_path_info) {
      auto name = store_path_to_name(get_s(fields, 0));
      i->s = fmt("querying " ANSI_BOLD "%s" ANSI_NORMAL " on %s", name, get_s(fields, 1));
    }

    if ((type == act_file_transfer && has_ancestor(*state, act_copy_path, parent)) ||
        (type == act_file_transfer && has_ancestor(*state, act_query_path_info, parent)) ||
        (type == act_copy_path && has_ancestor(*state, act_substitute, parent))) {
      i->visible = false;
    }

    update(*state);
  }

  /* Check whether an activity has an ancestor with the specified
     type. */
  bool has_ancestor(State& state, activity_type_t type, activity_id_t act) {
    while (act != 0) {
      auto i = state.its.find(act);
      if (i == state.its.end()) {
        break;
      }
      if (i->second->type == type) {
        return true;
      }
      act = i->second->parent;
    }
    return false;
  }

  void stop_activity(activity_id_t act) override {
    auto state(state_.lock());

    auto i = state->its.find(act);
    if (i != state->its.end()) {
      auto& act_by_type = state->activities_by_type[i->second->type];
      act_by_type.done += i->second->done;
      act_by_type.failed += i->second->failed;

      for (auto& j : i->second->expected_by_type) {
        state->activities_by_type[j.first].expected -= j.second;
      }

      act_by_type.its.erase(act);
      state->activities.erase(i->second);
      state->its.erase(i);
    }

    update(*state);
  }

  void result(activity_id_t act, result_type_t type, const std::vector<field_t>& fields) override {
    auto state(state_.lock());

    if (type == res_file_linked) {
      state->files_linked++;
      state->bytes_linked += get_i(fields, 0);
      update(*state);
    }

    else if (type == res_build_log_line || type == res_post_build_log_line) {
      auto last_line = chomp(get_s(fields, 0));
      auto i = state->its.find(act);
      assert(i != state->its.end());
      act_info_t info = *i->second;
      if (print_build_logs) {
        auto suffix = "> ";
        if (type == res_post_build_log_line) {
          suffix = " (post)> ";
        }
        log(*state, lvl_info,
            ANSI_FAINT + info.name.value_or("unnamed") + suffix + ANSI_NORMAL + last_line);
      } else {
        state->activities.erase(i->second);
        info.last_line = last_line;
        state->activities.emplace_back(info);
        i->second = std::prev(state->activities.end());
        update(*state);
      }
    }

    else if (type == res_untrusted_path) {
      state->untrusted_paths++;
      update(*state);
    }

    else if (type == res_corrupted_path) {
      state->corrupted_paths++;
      update(*state);
    }

    else if (type == res_set_phase) {
      auto i = state->its.find(act);
      assert(i != state->its.end());
      i->second->phase = get_s(fields, 0);
      update(*state);
    }

    else if (type == res_progress) {
      auto i = state->its.find(act);
      assert(i != state->its.end());
      act_info_t& act_info = *i->second;
      act_info.done = get_i(fields, 0);
      act_info.expected = get_i(fields, 1);
      act_info.running = get_i(fields, 2);
      act_info.failed = get_i(fields, 3);
      update(*state);
    }

    else if (type == res_set_expected) {
      auto i = state->its.find(act);
      assert(i != state->its.end());
      act_info_t& act_info = *i->second;
      auto type = (activity_type_t)get_i(fields, 0);
      auto& j = act_info.expected_by_type[type];
      state->activities_by_type[type].expected -= j;
      j = get_i(fields, 1);
      state->activities_by_type[type].expected += j;
      update(*state);
    }

    else if (type == res_fetch_status) {
      auto i = state->its.find(act);
      assert(i != state->its.end());
      act_info_t& act_info = *i->second;
      act_info.last_line = get_s(fields, 0);
      update(*state);
    }
  }

  void update(State& state) {
    state.have_update = true;
    updateCV.notify_one();
  }

  /**
   * Redraw, if the output has changed.
   *
   * Excessive redrawing is noticeable on slow terminals, and it interferes
   * with text selection in some terminals, including libvte-based terminal
   * emulators.
   */
  void redraw(std::string new_output) {
    auto last_output(lastOutput_.lock());
    if (new_output != *last_output) {
      write_to_stderr(new_output);
      *last_output = std::move(new_output);
    }
  }

  std::chrono::milliseconds draw(State& state) {
    auto next_wakeup = std::chrono::milliseconds::max();

    state.have_update = false;
    if (state.is_paused() || !state.active) {
      return next_wakeup;
    }

    std::string line;

    std::string status = get_status(state);
    if (!status.empty()) {
      line += '[';
      line += status;
      line += "]";
    }

    auto now = std::chrono::steady_clock::now();

    if (!state.activities.empty()) {
      if (!status.empty()) {
        line += " ";
      }
      auto i = state.activities.rbegin();

      while (i != state.activities.rend()) {
        if (i->visible && (!i->s.empty() || !i->last_line.empty())) {
          /* Don't show activities until some time has
             passed, to avoid displaying very short
             activities. */
          auto delay = std::chrono::milliseconds(10);
          if (i->start_time + delay < now) {
            break;
          } else {
            next_wakeup =
                std::min(next_wakeup, std::chrono::duration_cast<std::chrono::milliseconds>(
                                          delay - (now - i->start_time)));
          }
        }
        ++i;
      }

      if (i != state.activities.rend()) {
        line += i->s;
        if (!i->phase.empty()) {
          line += " (";
          line += i->phase;
          line += ")";
        }
        if (!i->last_line.empty()) {
          if (!i->s.empty()) {
            line += ": ";
          }
          line += i->last_line;
        }
      }
    }

    redraw("\r" + filter_ansi_escapes(line, false, get_window_width()) + ANSI_NORMAL + "\e[K");

    return next_wakeup;
  }

  std::string get_status(State& state) {
    std::string res;

    auto render_activity = [&] [[nodiscard]] (activity_type_t type, const std::string& item_fmt,
                                              const std::string& number_fmt = "%d",
                                              double unit = 1) {
      auto& act = state.activities_by_type[type];
      uint64_t done = act.done, expected = act.done, running = 0, failed = act.failed;
      for (auto& j : act.its) {
        done += j.second->done;
        expected += j.second->expected;
        running += j.second->running;
        failed += j.second->failed;
      }

      expected = std::max(expected, act.expected);

      std::string s;

      if (running || done || expected || failed) {
        if (running) {
          if (expected != 0) {
            s = fmt(ANSI_BLUE + number_fmt + ANSI_NORMAL "/" ANSI_GREEN + number_fmt +
                        ANSI_NORMAL "/" + number_fmt,
                    running / unit, done / unit, expected / unit);
          } else {
            s = fmt(ANSI_BLUE + number_fmt + ANSI_NORMAL "/" ANSI_GREEN + number_fmt + ANSI_NORMAL,
                    running / unit, done / unit);
          }
        } else if (expected != done) {
          if (expected != 0) {
            s = fmt(ANSI_GREEN + number_fmt + ANSI_NORMAL "/" + number_fmt, done / unit,
                    expected / unit);
          } else {
            s = fmt(ANSI_GREEN + number_fmt + ANSI_NORMAL, done / unit);
          }
        } else {
          s = fmt(done ? ANSI_GREEN + number_fmt + ANSI_NORMAL : number_fmt, done / unit);
        }
        s = fmt(item_fmt, s);

        if (failed) {
          s += fmt(" (" ANSI_RED "%d failed" ANSI_NORMAL ")", failed / unit);
        }
      }

      return s;
    };

    auto render_size_activity = [&] [[nodiscard]] (activity_type_t type,
                                                   const std::string& item_fmt = "%s") {
      auto& act = state.activities_by_type[type];
      uint64_t done = act.done, expected = act.done, running = 0, failed = act.failed;
      for (auto& j : act.its) {
        done += j.second->done;
        expected += j.second->expected;
        running += j.second->running;
        failed += j.second->failed;
      }

      expected = std::max(expected, act.expected);

      std::optional<SizeUnit> commonUnit;
      std::string s;

      if (running || done || expected || failed) {
        if (running) {
          if (expected != 0) {
            commonUnit = get_common_size_unit({(int64_t)running, (int64_t)done, (int64_t)expected});
            s = fmt(ANSI_BLUE "%s" ANSI_NORMAL "/" ANSI_GREEN "%s" ANSI_NORMAL "/%s",
                    commonUnit ? render_size_without_unit(running, *commonUnit)
                               : render_size(running),
                    commonUnit ? render_size_without_unit(done, *commonUnit) : render_size(done),
                    commonUnit ? render_size_without_unit(expected, *commonUnit)
                               : render_size(expected));
          } else {
            commonUnit = get_common_size_unit({(int64_t)running, (int64_t)done});
            s = fmt(ANSI_BLUE "%s" ANSI_NORMAL "/" ANSI_GREEN "%s" ANSI_NORMAL,
                    commonUnit ? render_size_without_unit(running, *commonUnit)
                               : render_size(running),
                    commonUnit ? render_size_without_unit(done, *commonUnit) : render_size(done));
          }
        } else if (expected != done) {
          if (expected != 0) {
            commonUnit = get_common_size_unit({(int64_t)done, (int64_t)expected});
            s = fmt(ANSI_GREEN "%s" ANSI_NORMAL "/%s",
                    commonUnit ? render_size_without_unit(done, *commonUnit) : render_size(done),
                    commonUnit ? render_size_without_unit(expected, *commonUnit)
                               : render_size(expected));
          } else {
            commonUnit = get_size_unit(done);
            s = fmt(ANSI_GREEN "%s" ANSI_NORMAL, render_size_without_unit(done, *commonUnit));
          }
        } else {
          commonUnit = get_size_unit(done);
          s = fmt(done ? ANSI_GREEN "%s" ANSI_NORMAL : "%s",
                  render_size_without_unit(done, *commonUnit));
        }

        if (commonUnit) {
          s = fmt("%s %siB", s, get_size_unit_suffix(*commonUnit));
        }

        s = fmt(item_fmt, s);

        if (failed) {
          s += fmt(" (" ANSI_RED "%s failed" ANSI_NORMAL ")", render_size(failed));
        }
      }

      return s;
    };

    auto maybe_append_to_result = [&](std::string_view s) {
      if (s.empty()) {
        return;
      }
      if (!res.empty()) {
        res += ", ";
      }
      res += s;
    };

    auto show_activity = [&](activity_type_t type, const std::string& item_fmt,
                             const std::string& number_fmt = "%d", double unit = 1) {
      maybe_append_to_result(render_activity(type, item_fmt, number_fmt, unit));
    };

    show_activity(act_builds, "%s built");

    auto s1 = render_activity(act_copy_paths, "%s copied");
    auto s2 = render_size_activity(act_copy_path);

    if (!s1.empty() || !s2.empty()) {
      if (!res.empty()) {
        res += ", ";
      }
      if (s1.empty()) {
        res += "0 copied";
      } else {
        res += s1;
      }
      if (!s2.empty()) {
        res += " (";
        res += s2;
        res += ')';
      }
    }

    maybe_append_to_result(render_size_activity(act_file_transfer, "%s DL"));

    {
      auto s = render_activity(act_optimise_store, "%s paths optimised");
      if (s != "") {
        s += fmt(", %s / %d inodes freed", render_size(state.bytes_linked), state.files_linked);
        if (!res.empty()) {
          res += ", ";
        }
        res += s;
      }
    }

    // FIXME: don't show "done" paths in green.
    show_activity(act_verify_paths, "%s paths verified");

    if (state.corrupted_paths) {
      if (!res.empty()) {
        res += ", ";
      }
      res += fmt(ANSI_RED "%d corrupted" ANSI_NORMAL, state.corrupted_paths);
    }

    if (state.untrusted_paths) {
      if (!res.empty()) {
        res += ", ";
      }
      res += fmt(ANSI_RED "%d untrusted" ANSI_NORMAL, state.untrusted_paths);
    }

    return res;
  }

  void write_to_stdout(std::string_view s) override {
    auto state(state_.lock());
    if (state->active) {
      std::cerr << "\r\e[K";
      logger_t::write_to_stdout(s);
      draw(*state);
    } else {
      logger_t::write_to_stdout(s);
    }
  }

  std::optional<char> ask(std::string_view msg) override {
    auto state(state_.lock());
    if (!state->active) {
      return {};
    }
    std::cerr << fmt("\r\e[K%s ", msg);
    auto s = trim(read_line(get_standard_input(), true));
    if (s.size() != 1) {
      return {};
    }
    draw(*state);
    return s[0];
  }

  void set_print_build_logs(bool print_build_logs) override {
    this->print_build_logs = print_build_logs;
  }
};

std::unique_ptr<logger_t> make_progress_bar() {
  return std::make_unique<progress_bar_t>(is_tty());
}

} // namespace nix
