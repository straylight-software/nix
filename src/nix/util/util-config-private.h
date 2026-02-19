// util-config-private.h
//
// Generated config header for nix-util library.
// Defines feature detection macros.

#pragma once

// Disable optional features for now - these would be detected by meson/cmake
#define HAVE_LIBCPUID 0
#define HAVE_ACL 0
#define HAVE_POSIX_FALLOCATE 1
