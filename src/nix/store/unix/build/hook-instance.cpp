#include "nix/store/build/hook-instance.h"

#include "nix/store/build/child.h"
#include "nix/store/globals.h"
#include "nix/util/config-global.h"
#include "nix/util/executable-path.h"
#include "nix/util/file-system.h"
#include "nix/util/strings.h"

namespace nix {

HookInstance::HookInstance() {
  debug("starting build hook '%s'", concat_strings_sep(" ", settings.buildHook.get()));

  auto buildHookArgs = settings.buildHook.get();

  if (buildHookArgs.empty())
    throw Error("'build-hook' setting is empty");

  std::filesystem::path buildHook = buildHookArgs.front();
  buildHookArgs.pop_front();

  try {
    buildHook = executable_path_t::load().find_path(buildHook);
  } catch (ExecutableLookupError& e) {
    e.add_trace(nullptr, "while resolving the 'build-hook' setting'");
    throw;
  }

  strings_t args;
  args.push_back(buildHook.filename().string());

  for (auto& arg : buildHookArgs)
    args.push_back(arg);

  args.push_back(std::to_string(verbosity));

  /* Create a pipe to get the output of the child. */
  fromHook.create();

  /* Create the communication pipes. */
  toHook.create();

  /* Create a pipe to get the output of the builder. */
  builder_out.create();

  /* Fork the hook. */
  pid = start_process([&]() {
    if (dup2(fromHook.write_side.get(), STDERR_FILENO) == -1)
      throw sys_error_t("cannot pipe standard error into log file");

    common_child_init();

    if (chdir("/") == -1)
      throw sys_error_t("changing into /");

    /* Dup the communication pipes. */
    if (dup2(toHook.read_side.get(), STDIN_FILENO) == -1)
      throw sys_error_t("dupping to-hook read side");

    /* use fd 4 for the builder's stdout/stderr. */
    if (dup2(builder_out.write_side.get(), 4) == -1)
      throw sys_error_t("dupping builder's stdout/stderr");

    /* Hack: pass the read side of that fd to allow build-remote
       to read SSH error messages. */
    if (dup2(builder_out.read_side.get(), 5) == -1)
      throw sys_error_t("dupping builder's stdout/stderr");

    execv(buildHook.native().c_str(), strings_to_char_ptrs(args).data());

    throw sys_error_t("executing '%s'", buildHook);
  });

  pid.set_separate_pg(true);
  fromHook.write_side = -1;
  toHook.read_side = -1;

  sink = fd_sink_t(toHook.write_side.get());
  std::map<std::string, config_t::setting_info_t> settings;
  global_config.get_settings(settings);
  for (auto& setting : settings)
    sink << 1 << setting.first << setting.second.value;
  sink << 0;
}

HookInstance::~HookInstance() {
  try {
    toHook.write_side = -1;
    if (pid != -1)
      pid.kill();
  } catch (...) {
    ignore_exception_in_destructor();
  }
}

} // namespace nix
