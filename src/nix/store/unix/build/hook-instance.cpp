#include "nix/store/build/hook-instance.h"

#include "nix/store/build/child.h"
#include "nix/store/globals.h"
#include "nix/util/config-global.h"
#include "nix/util/executable-path.h"
#include "nix/util/file-system.h"
#include "nix/util/strings.h"

namespace nix {

HookInstance::HookInstance() {
  debug("starting build hook '%s'", concatStringsSep(" ", settings.buildHook.get()));

  auto buildHookArgs = settings.buildHook.get();

  if (buildHookArgs.empty())
    throw Error("'build-hook' setting is empty");

  std::filesystem::path buildHook = buildHookArgs.front();
  buildHookArgs.pop_front();

  try {
    buildHook = executable_path_t::load().findPath(buildHook);
  } catch (ExecutableLookupError& e) {
    e.addTrace(nullptr, "while resolving the 'build-hook' setting'");
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
  builderOut.create();

  /* Fork the hook. */
  pid = startProcess([&]() {
    if (dup2(fromHook.writeSide.get(), STDERR_FILENO) == -1)
      throw sys_error_t("cannot pipe standard error into log file");

    commonChildInit();

    if (chdir("/") == -1)
      throw sys_error_t("changing into /");

    /* Dup the communication pipes. */
    if (dup2(toHook.readSide.get(), STDIN_FILENO) == -1)
      throw sys_error_t("dupping to-hook read side");

    /* Use fd 4 for the builder's stdout/stderr. */
    if (dup2(builderOut.writeSide.get(), 4) == -1)
      throw sys_error_t("dupping builder's stdout/stderr");

    /* Hack: pass the read side of that fd to allow build-remote
       to read SSH error messages. */
    if (dup2(builderOut.readSide.get(), 5) == -1)
      throw sys_error_t("dupping builder's stdout/stderr");

    execv(buildHook.native().c_str(), stringsToCharPtrs(args).data());

    throw sys_error_t("executing '%s'", buildHook);
  });

  pid.setSeparatePG(true);
  fromHook.writeSide = -1;
  toHook.readSide = -1;

  sink = fd_sink_t(toHook.writeSide.get());
  std::map<std::string, Config::setting_info_t> settings;
  globalConfig.getSettings(settings);
  for (auto& setting : settings)
    sink << 1 << setting.first << setting.second.value;
  sink << 0;
}

HookInstance::~HookInstance() {
  try {
    toHook.writeSide = -1;
    if (pid != -1)
      pid.kill();
  } catch (...) {
    ignoreExceptionInDestructor();
  }
}

} // namespace nix
