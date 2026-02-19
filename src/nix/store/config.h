// nix/store/config.h
//
// Generated config header for nix-store library.
// TODO[b7r6]: These should be detected by meson/cmake.

#ifndef NIX_STORE_CONFIG_H
#define NIX_STORE_CONFIG_H

// Feature flags - disable optional features for now to get clean build
// TODO[b7r6]: Enable these once we have the deps sorted
// NOLINTBEGIN(cppcoreguidelines-macro-to-enum,modernize-macro-to-enum)
#define NIX_WITH_AWS_AUTH 0
#define NIX_WITH_S3 0
#define NIX_WITH_GCS 0
// NOLINTEND(cppcoreguidelines-macro-to-enum,modernize-macro-to-enum)

// System configuration
// TODO[b7r6]: These should come from Nix configuration/detection
// NOLINTNEXTLINE(cppcoreguidelines-macro-usage)
#define NIX_LOCAL_SYSTEM "x86_64-linux"
// NOLINTNEXTLINE(cppcoreguidelines-macro-usage)
#define NIX_REMOTE_SYSTEMS ""
// NOLINTNEXTLINE(cppcoreguidelines-macro-usage)
#define SANDBOX_SHELL "/bin/sh"

// Linux has these
// NOLINTBEGIN(cppcoreguidelines-macro-to-enum,modernize-macro-to-enum,cppcoreguidelines-macro-usage)
#define HAVE_LCHOWN 1
#define HAVE_POSIX_FALLOCATE 1
#define HAVE_STATVFS 1
// NOLINTEND(cppcoreguidelines-macro-to-enum,modernize-macro-to-enum,cppcoreguidelines-macro-usage)

// Nix installation paths
// TODO[b7r6]: These should come from Nix configuration
// NOLINTNEXTLINE(cppcoreguidelines-macro-usage)
#define NIX_PREFIX "/nix"
// NOLINTNEXTLINE(cppcoreguidelines-macro-usage)
#define NIX_STORE_DIR "/nix/store"
// NOLINTNEXTLINE(cppcoreguidelines-macro-usage)
#define NIX_DATA_DIR "/nix/var/nix"
// NOLINTNEXTLINE(cppcoreguidelines-macro-usage)
#define NIX_LOG_DIR "/nix/var/log/nix"
// NOLINTNEXTLINE(cppcoreguidelines-macro-usage)
#define NIX_STATE_DIR "/nix/var/nix"
// NOLINTNEXTLINE(cppcoreguidelines-macro-usage)
#define NIX_CONF_DIR "/etc/nix"

// Version info
// TODO[b7r6]: Get this from git/build
// NOLINTNEXTLINE(cppcoreguidelines-macro-usage)
#define PACKAGE_VERSION "2.99.0-straylight"
// NOLINTNEXTLINE(cppcoreguidelines-macro-usage)
#define DETERMINATE_NIX_VERSION ""

#endif // NIX_STORE_CONFIG_H
