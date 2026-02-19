// util-unix-config-private.h
//
// Generated config header for Unix-specific nix-util code.
// TODO[b7r6]: These should be detected by meson/cmake, for now we assume modern Linux.

#pragma once

// Linux has these
#define HAVE_PIPE2 1
#define HAVE_STRSIGNAL 1
#define HAVE_SYSCONF 1
#define HAVE_UTIMENSAT 1
#define HAVE_DECL_AT_SYMLINK_NOFOLLOW 1
#define HAVE_LUTIMES 1
#define HAVE_CLOSE_RANGE 1

// Linux 5.6+ has openat2
#if __has_include(<linux/openat2.h>)
#  define HAVE_OPENAT2 1
#else
#  define HAVE_OPENAT2 0
#endif
