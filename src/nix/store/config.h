// nix/store/config.h
//
// Generated config header for nix-store library.
// TODO[b7r6]: These should be detected by meson/cmake.

#pragma once

// Feature flags - disable optional features for now to get clean build
// TODO[b7r6]: Enable these once we have the deps sorted
#define NIX_WITH_AWS_AUTH 0
#define NIX_WITH_S3 0
#define NIX_WITH_GCS 0

// System configuration
// TODO[b7r6]: These should come from Nix configuration/detection
#define NIX_LOCAL_SYSTEM "x86_64-linux"
#define NIX_REMOTE_SYSTEMS ""
#define SANDBOX_SHELL "/bin/sh"

// Linux has these
#define HAVE_LCHOWN 1
#define HAVE_POSIX_FALLOCATE 1
#define HAVE_STATVFS 1

// Nix installation paths
// TODO[b7r6]: These should come from Nix configuration
#define NIX_PREFIX "/nix"
#define NIX_STORE_DIR "/nix/store"
#define NIX_DATA_DIR "/nix/var/nix"
#define NIX_LOG_DIR "/nix/var/log/nix"
#define NIX_STATE_DIR "/nix/var/nix"
#define NIX_CONF_DIR "/etc/nix"

// Version info
// TODO[b7r6]: Get this from git/build
#define PACKAGE_VERSION "2.99.0-straylight"
#define DETERMINATE_NIX_VERSION ""
