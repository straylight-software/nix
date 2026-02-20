/* Generated configuration for libgit2 - Linux/musl with LibreSSL */
#ifndef INCLUDE_features_h__
#define INCLUDE_features_h__

/* Threading enabled */
#define GIT_THREADS 1

/* 64-bit architecture */
#define GIT_ARCH_64 1

/* Use nanosecond timestamps */
#define GIT_USE_NSEC 1
#define GIT_USE_STAT_MTIM 1
#define GIT_USE_FUTIMENS 1

/* Use builtin PCRE regex */
#define GIT_REGEX_BUILTIN 1

/* Use GNU qsort_r */
#define GIT_QSORT_GNU 1

/* SSH via exec (no libssh2) */
#define GIT_SSH 1
#define GIT_SSH_EXEC 1

/* HTTPS via OpenSSL (LibreSSL is compatible) */
#define GIT_HTTPS 1
#define GIT_OPENSSL 1

/* HTTP parser - use bundled llhttp */
#define GIT_HTTPPARSER_LLHTTP 1

/* SHA1 - use collision detection for security */
#define GIT_SHA1_COLLISIONDETECT 1

/* SHA256 - use OpenSSL (LibreSSL) */
#define GIT_SHA256_OPENSSL 1

/* Compression - use bundled zlib */
#define GIT_COMPRESSION_BUILTIN 1

/* Random - use getentropy (available in musl) */
#define GIT_RAND_GETENTROPY 1

/* I/O - use poll */
#define GIT_IO_POLL 1

#endif
