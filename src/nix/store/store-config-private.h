// store-config-private.h
//
// Generated config header for nix-store library.
// TODO[b7r6]: These should be detected by meson/cmake.

#pragma once

// Linux has these
#define HAVE_LCHOWN 1
#define HAVE_POSIX_FALLOCATE 1
#define HAVE_STATVFS 1

// System-specific paths
// TODO[b7r6]: These should come from Nix configuration
#define SANDBOX_SHELL "/bin/sh"
#define NIX_LOCAL_SYSTEM "x86_64-linux"
#define NIX_REMOTE_SYSTEMS ""
