#include "nix/store/globals.h"

#include <algorithm>
#include <map>
#include <mutex>
#include <thread>

#include <curl/curl.h>

#include <nlohmann/json.hpp>

#include "nix/util/abstract-setting-to-json.h"
#include "nix/util/archive.h"
#include "nix/util/args.h"
#include "nix/util/compute-levels.h"
#include "nix/util/config-global.h"
#include "nix/util/current-process.h"
#include "nix/util/signals.h"

#ifndef _WIN32
#  include <sys/utsname.h>
#endif

#ifdef __GLIBC__
#  include <dlfcn.h>
#  include <gnu/lib-names.h>
#  include <nss.h>
#endif

#ifdef __APPLE__
#  include "nix/util/processes.h"
#endif

#include "nix/util/config-impl.h"

#ifdef __APPLE__
#  include <sys/sysctl.h>
#endif

#include "store-config-private.h"

namespace nix {

/* The default location of the daemon socket, relative to nixStateDir.
   The socket is in a directory to allow you to control access to the
   Nix daemon by setting the mode/ownership of the directory
   appropriately.  (This wouldn't work on the socket itself since it
   must be deleted and recreated on startup.) */
#define DEFAULT_SOCKET_PATH "/daemon-socket/socket"

settings_t settings;

static global_config_t::Register r_settings(&settings);

settings_t::settings_t()
    : nixPrefix(NIX_PREFIX),
      nixStore(
#ifndef _WIN32
          // On Windows `/nix/store` is not a canonical path, but we dont'
          // want to deal with that yet.
          canon_path
#endif
          (get_env_non_empty("NIX_STORE_DIR")
               .value_or(get_env_non_empty("NIX_STORE").value_or(NIX_STORE_DIR)))),
      nixDataDir(canon_path(get_env_non_empty("NIX_DATA_DIR").value_or(NIX_DATA_DIR))),
      nixLogDir(canon_path(get_env_non_empty("NIX_LOG_DIR").value_or(NIX_LOG_DIR))),
      nixStateDir(canon_path(get_env_non_empty("NIX_STATE_DIR").value_or(NIX_STATE_DIR))),
      nixConfDir(canon_path(get_env_non_empty("NIX_CONF_DIR").value_or(NIX_CONF_DIR))),
      nixUserConfFiles(get_user_config_files()),
      nixDaemonSocketFile(canon_path(
          get_env_non_empty("NIX_DAEMON_SOCKET_PATH").value_or(nixStateDir + DEFAULT_SOCKET_PATH))) {
#ifndef _WIN32
  buildUsersGroup = is_root_user() ? "nixbld" : "";
#endif
  allowSymlinkedStore = get_env("NIX_IGNORE_SYMLINK_STORE") == "1";

  auto sslOverride = get_env("NIX_SSL_CERT_FILE").value_or(get_env("SSL_CERT_FILE").value_or(""));
  if (sslOverride != "")
    ca_file = sslOverride;

  /* Backwards compatibility. */
  auto s = get_env("NIX_REMOTE_SYSTEMS");
  if (s) {
    strings_t ss;
    for (auto& p : tokenize_string<strings_t>(*s, ":"))
      ss.push_back("@" + p);
    builders = concat_strings_sep("\n", ss);
  }

#if (defined(__linux__) || defined(__FreeBSD__)) && defined(SANDBOX_SHELL)
  sandboxPaths = {{"/bin/sh", {.source = SANDBOX_SHELL}}};
#endif

  /* chroot-like behavior from Apple's sandbox */
#ifdef __APPLE__
  for (path_view_t p : {
           "/System/Library/Frameworks",
           "/System/Library/PrivateFrameworks",
           "/bin/sh",
           "/bin/bash",
           "/private/tmp",
           "/private/var/tmp",
           "/usr/lib",
       }) {
    sandboxPaths.get().insert_or_assign(std::string{p}, ChrootPath{.source = std::string{p}});
  }
  allowedImpureHostPrefixes = tokenize_string<string_set_t>("/System/Library /usr/lib /dev /bin/sh");
#endif
}

void load_conf_file(abstract_config_t& config) {
  auto apply_config_file = [&](const Path& path) {
    try {
      std::string contents = read_file(path);
      config.apply_config(contents, path);
    } catch (SystemError&) {
    }
  };

  apply_config_file((settings.nixConfDir / "nix.conf").string());

  /* We only want to send overrides to the daemon, i.e. stuff from
     ~/.nix/nix.conf or the command line. */
  config.reset_overridden();

  auto files = settings.nixUserConfFiles;
  for (auto file = files.rbegin(); file != files.rend(); file++) {
    apply_config_file(*file);
  }

  auto nix_conf_env = get_env("NIX_CONFIG");
  if (nix_conf_env.has_value()) {
    config.apply_config(nix_conf_env.value(), "NIX_CONFIG");
  }
}

std::vector<Path> get_user_config_files() {
  // Use the paths specified in NIX_USER_CONF_FILES if it has been defined
  auto nix_conf_files = get_env("NIX_USER_CONF_FILES");
  if (nix_conf_files.has_value()) {
    return tokenize_string<std::vector<std::string>>(nix_conf_files.value(), ":");
  }

  // Use the paths specified by the XDG spec
  std::vector<Path> files;
  auto dirs = get_config_dirs();
  for (auto& dir : dirs) {
    files.insert(files.end(), (dir / "nix.conf").string());
  }
  return files;
}

unsigned int settings_t::getDefaultCores() {
  const unsigned int concurrency = std::max(1U, std::thread::hardware_concurrency());
  const unsigned int maxCPU = get_max_cpu();

  if (maxCPU > 0)
    return maxCPU;
  else
    return concurrency;
}

#ifdef __APPLE__
static bool hasVirt() {
  int hasVMM;
  int hvSupport;
  size_t size;

  size = sizeof(hasVMM);
  if (sysctlbyname("kern.hv_vmm_present", &hasVMM, &size, NULL, 0) == 0) {
    if (hasVMM)
      return false;
  }

  // whether the kernel and hardware supports virt
  size = sizeof(hvSupport);
  if (sysctlbyname("kern.hv_support", &hvSupport, &size, NULL, 0) == 0) {
    return hvSupport == 1;
  } else {
    return false;
  }
}
#endif

string_set_t settings_t::getDefaultSystemFeatures() {
  /* For backwards compatibility, accept some "features" that are
     used in Nixpkgs to route builds to certain machines but don't
     actually require anything special on the machines. */
  string_set_t features{"nixos-test", "benchmark", "big-parallel"};

#ifdef __linux__
  features.insert("uid-range");
#endif

#ifdef __linux__
  if (access("/dev/kvm", R_OK | W_OK) == 0)
    features.insert("kvm");
#endif

#ifdef __APPLE__
  if (hasVirt())
    features.insert("apple-virt");
#endif

  return features;
}

string_set_t settings_t::getDefaultExtraPlatforms() {
  string_set_t extraPlatforms;

  if (std::string{NIX_LOCAL_SYSTEM} == "x86_64-linux" && !isWSL1())
    extraPlatforms.insert("i686-linux");

#ifdef __linux__
  string_set_t levels = compute_levels();
  for (auto iter = levels.begin(); iter != levels.end(); ++iter)
    extraPlatforms.insert(*iter + "-linux");
#elif defined(__APPLE__)
  // Rosetta 2 emulation layer can run x86_64 binaries on aarch64
  // machines. Note that we can’t force processes from executing
  // x86_64 in aarch64 environments or vice versa since they can
  // always exec with their own binary preferences.
  if (std::string{NIX_LOCAL_SYSTEM} == "aarch64-darwin" &&
      run_program(run_options_t{.program = "arch",
                            .args = {"-arch", "x86_64", "/usr/bin/true"},
                            .merge_stderr_to_stdout = true})
              .first == 0)
    extraPlatforms.insert("x86_64-darwin");
#endif

  return extraPlatforms;
}

bool settings_t::isWSL1() {
#ifdef __linux__
  struct utsname utsbuf;
  uname(&utsbuf);
  // WSL1 uses -Microsoft suffix
  // WSL2 uses -microsoft-standard suffix
  return has_suffix(utsbuf.release, "-Microsoft");
#else
  return false;
#endif
}

Path settings_t::getDefaultSSLCertFile() {
  for (auto& fn : {"/etc/ssl/certs/ca-certificates.crt",
                   "/nix/var/nix/profiles/default/etc/ssl/certs/ca-bundle.crt"})
    if (path_accessible(fn))
      return fn;
  return "";
}

const ExternalBuilder* settings_t::findExternalDerivationBuilderIfSupported(const derivation_t& drv) {
  if (auto it = std::ranges::find_if(
          externalBuilders.get(),
          [&](const auto& handler) { return handler.systems.contains(drv.platform); });
      it != externalBuilders.get().end())
    return &*it;
  return nullptr;
}

std::string nix_version = PACKAGE_VERSION;

const std::string determinate_nix_version = DETERMINATE_NIX_VERSION;

NLOHMANN_JSON_SERIALIZE_ENUM(SandboxMode, {
                                              {SandboxMode::smEnabled, true},
                                              {SandboxMode::smRelaxed, "relaxed"},
                                              {SandboxMode::smDisabled, false},
                                          });

template <>
SandboxMode base_setting_t<SandboxMode>::parse(const std::string& str) const {
  if (str == "true")
    return smEnabled;
  else if (str == "relaxed")
    return smRelaxed;
  else if (str == "false")
    return smDisabled;
  else
    throw UsageError("option '%s' has invalid value_ '%s'", name, str);
}

template <>
struct base_setting_t<SandboxMode>::trait {
  static constexpr bool appendable = false;
};

template <>
std::string base_setting_t<SandboxMode>::to_string() const {
  if (value_ == smEnabled)
    return "true";
  else if (value_ == smRelaxed)
    return "relaxed";
  else if (value_ == smDisabled)
    return "false";
  else
    unreachable();
}

template <>
void base_setting_t<SandboxMode>::convert_to_arg(args_t& args, const std::string& category) {
  args.add_flag({
      .long_name = name,
      .aliases = aliases,
      .description = "Enable sandboxing.",
      .category = category,
      .handler = {[this]() { override(smEnabled); }},
  });
  args.add_flag({
      .long_name = "no-" + name,
      .aliases = aliases,
      .description = "Disable sandboxing.",
      .category = category,
      .handler = {[this]() { override(smDisabled); }},
  });
  args.add_flag({
      .long_name = "relaxed-" + name,
      .aliases = aliases,
      .description = "Enable sandboxing, but allow builds to disable it.",
      .category = category,
      .handler = {[this]() { override(smRelaxed); }},
  });
}

NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(ChrootPath, source, optional)

template <>
PathsInChroot base_setting_t<PathsInChroot>::parse(const std::string& str) const {
  PathsInChroot paths_in_chroot;
  for (auto i : tokenize_string<string_set_t>(str)) {
    if (i.empty())
      continue;
    bool optional = false;
    if (i[i.size() - 1] == '?') {
      optional = true;
      i.pop_back();
    }
    size_t p = i.find('=');
    std::string inside, outside;
    if (p == std::string::npos) {
      inside = i;
      outside = i;
    } else {
      inside = i.substr(0, p);
      outside = i.substr(p + 1);
    }
    paths_in_chroot[inside] = {.source = outside, .optional = optional};
  }
  return paths_in_chroot;
}

template <>
std::string base_setting_t<PathsInChroot>::to_string() const {
  std::vector<std::string> accum;
  for (auto& [name, cp] : value_) {
    std::string s = name == cp.source ? name : name + "=" + cp.source;
    if (cp.optional)
      s += "?";
    accum.push_back(std::move(s));
  }
  return concat_strings_sep(" ", accum);
}

unsigned int MaxBuildJobsSetting::parse(const std::string& str) const {
  if (str == "auto")
    return std::max(1U, std::thread::hardware_concurrency());
  else {
    if (auto n = string2_int<decltype(value_)>(str))
      return *n;
    else
      throw UsageError("configuration setting '%s' should be 'auto' or an integer", name);
  }
}

template <>
settings_t::external_builders
base_setting_t<settings_t::external_builders>::parse(const std::string& str) const {
  try {
    return nlohmann::json::parse(str).template get<settings_t::external_builders>();
  } catch (std::exception& e) {
    throw UsageError("parsing setting '%s': %s", name, e.what());
  }
}

template <>
std::string base_setting_t<settings_t::external_builders>::to_string() const {
  return nlohmann::json(value_).dump();
}

template <>
void base_setting_t<PathsInChroot>::append_or_set(PathsInChroot new_value, bool append) {
  if (!append)
    value_.clear();
  value_.insert(std::make_move_iterator(new_value.begin()), std::make_move_iterator(new_value.end()));
}

static void preloadNSS() {
  /* builtin:fetchurl can trigger a DNS lookup, which with glibc can trigger a dynamic library load
     of one of the glibc NSS libraries in a sandboxed child, which will fail unless the library's
     already been loaded in the parent. So we force a lookup of an invalid domain to force the NSS
     machinery to load its lookup libraries in the parent before any child gets a chance to. */
  static std::once_flag dns_resolve_flag;

  std::call_once(dns_resolve_flag, []() {
#ifdef __GLIBC__
    /* On linux, glibc will run every lookup through the nss layer.
     * That means every lookup goes, by default, through nscd, which acts as a local
     * cache.
     * Because we run builds in a sandbox, we also remove access to nscd otherwise
     * lookups would leak into the sandbox.
     *
     * But now we have a new problem, we need to make sure the nss_dns backend that
     * does the dns lookups when nscd is not available is loaded or available.
     *
     * We can't make it available without leaking nix's environment, so instead we'll
     * load the backend, and configure nss so it does not try to run dns lookups
     * through nscd.
     *
     * This is technically only used for builtins:fetch* functions so we only care
     * about dns.
     *
     * All other platforms are unaffected.
     */
    if (!dlopen(LIBNSS_DNS_SO, RTLD_NOW))
      warn("unable to load nss_dns backend");
    // FIXME: get hosts entry from nsswitch.conf.
    __nss_configure_lookup("hosts", "files dns");
#endif
  });
}

static bool initLibStoreDone = false;

void assert_lib_store_initialized() {
  if (!initLibStoreDone) {
    printError(
        "The program must call nix::initNix() before calling any libstore library functions.");
    abort();
  };
}

void init_lib_store(bool load_config) {
  if (initLibStoreDone)
    return;

  init_lib_util();

  if (load_config)
    load_conf_file(global_config);

  preloadNSS();

  /* Because of an objc quirk[1], calling curl_global_init for the first time
     after fork() will always result in a crash.
     Up until now the solution has been to set OBJC_DISABLE_INITIALIZE_FORK_SAFETY
     for every nix process to ignore that error.
     Instead of working around that error we address it at the core -
     by calling curl_global_init here, which should mean curl will already
     have been initialized by the time we try to do so in a forked process.

     [1]
     https://github.com/apple-oss-distributions/objc4/blob/01edf1705fbc3ff78a423cd21e03dfc21eb4d780/runtime/objc-initialize.mm#L614-L636
  */
  curl_global_init(CURL_GLOBAL_ALL);
#ifdef __APPLE__
  /* On macOS, don't use the per-session TMPDIR (as set e.g. by
     sshd). This breaks build users because they don't have access
     to the TMPDIR, in particular in ‘nix-store --serve’. */
  if (has_prefix(default_temp_dir().string(), "/var/folders/"))
    unsetenv("TMPDIR");
#endif

  initLibStoreDone = true;
}

} // namespace nix
