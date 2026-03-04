/*
 * nix-builder-init - Guest init for Firecracker build VMs
 *
 * This program runs as PID 1 inside the microVM and:
 * 1. Sets up minimal filesystem (mount /proc, /dev, etc.)
 * 2. Connects to host via vsock
 * 3. Receives BUILD_EXEC commands
 * 4. Executes builders and streams output back
 * 5. Reports BUILD_EXIT with result
 *
 * Wire protocol matches vm_protocol.h
 *
 * Build: musl-gcc -static -O2 -o nix-builder-init nix-builder-init.c
 */

#define _GNU_SOURCE

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <linux/reboot.h>
#include <sys/socket.h>
/*
 * Define vsock constants and structures ourselves to avoid compatibility
 * issues with linux/vm_sockets.h and musl's newer linux-headers.
 * The linux/vm_sockets.h header uses sizeof(struct sockaddr) in a struct
 * definition which doesn't work with musl.
 */
#ifndef AF_VSOCK
#  define AF_VSOCK 40
#endif
#define VMADDR_CID_ANY ((unsigned int)-1)
#define VMADDR_CID_HOST 2

struct sockaddr_vm {
  sa_family_t svm_family;
  unsigned short svm_reserved1;
  unsigned int svm_port;
  unsigned int svm_cid;
  unsigned char svm_zero[sizeof(struct sockaddr) - sizeof(sa_family_t) - sizeof(unsigned short) -
                         sizeof(unsigned int) - sizeof(unsigned int)];
};
#include <signal.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mount.h>
#include <sys/prctl.h>
#include <sys/reboot.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/sysmacros.h>
#include <sys/wait.h>
#include <unistd.h>

/* ============================================================================
 * Wire Protocol Constants (must match vm_protocol.h)
 * ============================================================================ */

#define VM_PROTOCOL_MAGIC 0x4E495842 /* "NIXB" */
#define VM_PROTOCOL_VERSION 1

#define VM_VSOCK_HOST_CID 2
#define VM_VSOCK_BUILD_PORT 5000

/* Message types */
#define MSG_BUILD_EXEC 0x0001
#define MSG_BUILD_ABORT 0x0002
#define MSG_PING 0x0003
#define MSG_BUILD_STDOUT 0x0101
#define MSG_BUILD_STDERR 0x0102
#define MSG_BUILD_EXIT 0x0103
#define MSG_PONG 0x0104
#define MSG_WITNESS_EVENT 0x0105

/* Wire header - 12 bytes */
struct wire_header {
  uint32_t magic;
  uint16_t version;
  uint16_t msg_type;
  uint32_t payload_len;
} __attribute__((packed));

/* ============================================================================
 * Logging
 * ============================================================================ */

static void log_msg(const char* level, const char* fmt, ...) {
  va_list ap;
  fprintf(stderr, "[%s] nix-builder-init: ", level);
  va_start(ap, fmt);
  vfprintf(stderr, fmt, ap);
  va_end(ap);
  fprintf(stderr, "\n");
}

#define log_info(...) log_msg("INFO", __VA_ARGS__)
#define log_error(...) log_msg("ERROR", __VA_ARGS__)

/* ============================================================================
 * Filesystem Setup
 * ============================================================================ */

static void setup_filesystem(void) {
  /* Mount proc */
  mkdir("/proc", 0755);
  if (mount("proc", "/proc", "proc", MS_NOEXEC | MS_NOSUID | MS_NODEV, NULL) < 0) {
    log_error("mount /proc failed: %s", strerror(errno));
  }

  /* Mount devtmpfs */
  mkdir("/dev", 0755);
  if (mount("devtmpfs", "/dev", "devtmpfs", MS_NOSUID, NULL) < 0) {
    /* Try tmpfs if devtmpfs not available */
    if (mount("tmpfs", "/dev", "tmpfs", MS_NOSUID, "mode=755") < 0) {
      log_error("mount /dev failed: %s", strerror(errno));
    }
    /* Create basic devices */
    mknod("/dev/null", S_IFCHR | 0666, makedev(1, 3));
    mknod("/dev/zero", S_IFCHR | 0666, makedev(1, 5));
    mknod("/dev/urandom", S_IFCHR | 0666, makedev(1, 9));
    mknod("/dev/tty", S_IFCHR | 0666, makedev(5, 0));
  }

  /* Create block devices for virtio drives */
  mknod("/dev/vda", S_IFBLK | 0660, makedev(254, 0));  /* store image */
  mknod("/dev/vdb", S_IFBLK | 0660, makedev(254, 16)); /* output image */

  /* Create /dev/fd and standard stream symlinks (needed by bash process substitution)
   * These point to /proc/self/fd which provides per-process file descriptor access */
  symlink("/proc/self/fd", "/dev/fd");
  symlink("/proc/self/fd/0", "/dev/stdin");
  symlink("/proc/self/fd/1", "/dev/stdout");
  symlink("/proc/self/fd/2", "/dev/stderr");

  /* Create /dev/ptmx for pseudo-terminals (some builds need this) */
  mknod("/dev/ptmx", S_IFCHR | 0666, makedev(5, 2));
  mkdir("/dev/pts", 0755);
  mount("devpts", "/dev/pts", "devpts", 0, "newinstance,ptmxmode=0666");

  /* Mount sysfs */
  mkdir("/sys", 0755);
  mount("sysfs", "/sys", "sysfs", MS_NOEXEC | MS_NOSUID | MS_NODEV, NULL);

  /* Create /tmp */
  mkdir("/tmp", 01777);
  mount("tmpfs", "/tmp", "tmpfs", MS_NOSUID | MS_NODEV, "mode=1777");

  /* Create /build directory for derivation builds */
  mkdir("/build", 0755);

  /* Create directory structure */
  mkdir("/nix", 0755);
  mkdir("/nix/store", 0755);
  mkdir("/nix-lower", 0755); /* For overlay lowerdir */
  mkdir("/output", 0755);

  /* Mount store image (read-only) at /nix-lower for overlay lowerdir
   * The host populates this with all required input paths */
  if (mount("/dev/vda", "/nix-lower", "ext4", MS_RDONLY | MS_NOATIME, NULL) < 0) {
    log_error("mount /nix-lower (store inputs) failed: %s", strerror(errno));
    log_error("builds will fail without /nix/store access");
  } else {
    log_info("mounted /nix/store from /dev/vda (read-only)");
  }

  /* Mount output image (read-write) from /dev/vdb
   * This is where build outputs are written */
  if (mount("/dev/vdb", "/output", "ext4", MS_NOATIME, NULL) < 0) {
    log_error("mount /output failed: %s", strerror(errno));
    log_error("builds will fail without output storage");
  } else {
    log_info("mounted /output from /dev/vdb (read-write)");
    /* Create output directories */
    mkdir("/output/nix", 0755);
    mkdir("/output/nix/store", 0755);
    mkdir("/output/work", 0755); /* overlay workdir - must be on same fs as upperdir */
  }

  /* Set up overlay so /nix/store shows inputs (read-only) but allows writes to /output
   * lowerdir: read-only input paths from /dev/vda
   * upperdir: writable output area on /dev/vdb
   * workdir: overlay work directory on /dev/vdb (same fs as upperdir) */
  char overlay_opts[512];
  snprintf(overlay_opts, sizeof(overlay_opts),
           "lowerdir=/nix-lower/store,upperdir=/output/nix/store,workdir=/output/work");

  if (mount("overlay", "/nix/store", "overlay", 0, overlay_opts) < 0) {
    log_error("overlay mount failed: %s - falling back to simple mount", strerror(errno));
    /* Fallback: just mount the store read-only, outputs won't work */
    if (mount("/dev/vda", "/nix", "ext4", MS_RDONLY | MS_NOATIME, NULL) < 0) {
      log_error("fallback mount also failed: %s", strerror(errno));
    }
  } else {
    log_info("mounted /nix/store as overlay (inputs from vda, outputs to vdb)");
  }

  log_info("filesystem setup complete");
}

/* ============================================================================
 * vsock Communication
 * ============================================================================ */

static int vsock_fd = -1;
static int vsock_listen_fd = -1;

static int vsock_listen_and_accept(void) {
  /* Create listening socket */
  vsock_listen_fd = socket(AF_VSOCK, SOCK_STREAM, 0);
  if (vsock_listen_fd < 0) {
    log_error("socket(AF_VSOCK) failed: %s", strerror(errno));
    return -1;
  }

  /* Bind to VMADDR_CID_ANY so we accept from any CID (the host) */
  struct sockaddr_vm addr = {
      .svm_family = AF_VSOCK,
      .svm_cid = VMADDR_CID_ANY,
      .svm_port = VM_VSOCK_BUILD_PORT,
  };

  if (bind(vsock_listen_fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
    log_error("bind() failed: %s", strerror(errno));
    close(vsock_listen_fd);
    vsock_listen_fd = -1;
    return -1;
  }

  if (listen(vsock_listen_fd, 1) < 0) {
    log_error("listen() failed: %s", strerror(errno));
    close(vsock_listen_fd);
    vsock_listen_fd = -1;
    return -1;
  }

  log_info("listening on vsock port %d, waiting for host connection...", VM_VSOCK_BUILD_PORT);

  /* Accept connection from host (via Firecracker's vsock proxy) */
  struct sockaddr_vm peer_addr;
  socklen_t peer_len = sizeof(peer_addr);
  vsock_fd = accept(vsock_listen_fd, (struct sockaddr*)&peer_addr, &peer_len);
  if (vsock_fd < 0) {
    log_error("accept() failed: %s", strerror(errno));
    close(vsock_listen_fd);
    vsock_listen_fd = -1;
    return -1;
  }

  log_info("accepted connection from host (CID %u)", peer_addr.svm_cid);

  /* Close listening socket - we only handle one connection */
  close(vsock_listen_fd);
  vsock_listen_fd = -1;

  return 0;
}

static ssize_t read_exact(void* buf, size_t len) {
  size_t total = 0;
  while (total < len) {
    ssize_t n = read(vsock_fd, (char*)buf + total, len - total);
    if (n < 0) {
      if (errno == EINTR) {
        continue;
      }
      return -1;
    }
    if (n == 0) {
      return -1; /* EOF */
    }
    total += n;
  }
  return (ssize_t)total;
}

static ssize_t write_all(const void* buf, size_t len) {
  size_t total = 0;
  while (total < len) {
    ssize_t n = write(vsock_fd, (const char*)buf + total, len - total);
    if (n < 0) {
      if (errno == EINTR) {
        continue;
      }
      return -1;
    }
    total += n;
  }
  return (ssize_t)total;
}

static int send_message(uint16_t msg_type, const void* payload, uint32_t len) {
  struct wire_header hdr = {
      .magic = VM_PROTOCOL_MAGIC,
      .version = VM_PROTOCOL_VERSION,
      .msg_type = msg_type,
      .payload_len = len,
  };

  if (write_all(&hdr, sizeof(hdr)) < 0) {
    return -1;
  }
  if (len > 0 && write_all(payload, len) < 0) {
    return -1;
  }
  return 0;
}

static int recv_header(struct wire_header* hdr) {
  if (read_exact(hdr, sizeof(*hdr)) < 0) {
    return -1;
  }
  if (hdr->magic != VM_PROTOCOL_MAGIC || hdr->version != VM_PROTOCOL_VERSION) {
    log_error("invalid header: magic=0x%x version=%d", hdr->magic, hdr->version);
    return -1;
  }
  return 0;
}

/* ============================================================================
 * Message Parsing
 * ============================================================================ */

/* Read a length-prefixed string from buffer */
static char* read_string(const uint8_t** ptr, const uint8_t* end) {
  if (*ptr + 4 > end) {
    return NULL;
  }
  uint32_t len = (*ptr)[0] | ((*ptr)[1] << 8) | ((*ptr)[2] << 16) | ((*ptr)[3] << 24);
  *ptr += 4;
  if (*ptr + len > end) {
    return NULL;
  }
  char* str = malloc(len + 1);
  if (!str) {
    return NULL;
  }
  memcpy(str, *ptr, len);
  str[len] = '\0';
  *ptr += len;
  return str;
}

/* Read a length-prefixed blob from buffer, returning both data and length.
 * Used for file contents that may contain embedded NULs. */
static char* read_blob(const uint8_t** ptr, const uint8_t* end, size_t* out_len) {
  if (*ptr + 4 > end) {
    return NULL;
  }
  uint32_t len = (*ptr)[0] | ((*ptr)[1] << 8) | ((*ptr)[2] << 16) | ((*ptr)[3] << 24);
  *ptr += 4;
  if (*ptr + len > end) {
    return NULL;
  }
  char* data = malloc(len);
  if (!data) {
    return NULL;
  }
  memcpy(data, *ptr, len);
  *ptr += len;
  *out_len = len;
  return data;
}

/* Extra file to create in workdir before exec */
struct extra_file {
  char* filename;
  char* contents;
  size_t contents_len;
};

/* Build request parsed from BUILD_EXEC payload */
struct build_request {
  char* builder;
  char** args;
  int argc;
  char** env;
  int envc;
  char* workdir;
  char** outputs;
  int output_count;
  struct extra_file* extra_files;
  int extra_file_count;
};

static void free_build_request(struct build_request* req) {
  free(req->builder);
  for (int i = 0; i < req->argc; i++) {
    free(req->args[i]);
  }
  free(req->args);
  /* env array has envc entries (each is "KEY=value"), NOT envc*2 */
  for (int i = 0; i < req->envc; i++) {
    free(req->env[i]);
  }
  free(req->env);
  free(req->workdir);
  for (int i = 0; i < req->output_count; i++) {
    free(req->outputs[i]);
  }
  free(req->outputs);
  for (int i = 0; i < req->extra_file_count; i++) {
    free(req->extra_files[i].filename);
    free(req->extra_files[i].contents);
  }
  free(req->extra_files);
}

static int parse_build_request(const uint8_t* data, size_t len, struct build_request* req) {
  const uint8_t* ptr = data;
  const uint8_t* end = data + len;

  memset(req, 0, sizeof(*req));

  /* Builder path */
  req->builder = read_string(&ptr, end);
  if (!req->builder) {
    return -1;
  }

  /* Args */
  if (ptr + 4 > end) {
    return -1;
  }
  req->argc = ptr[0] | (ptr[1] << 8) | (ptr[2] << 16) | (ptr[3] << 24);
  ptr += 4;
  req->args = calloc(req->argc + 2, sizeof(char*)); /* +2 for builder and NULL */
  if (!req->args) {
    return -1;
  }
  req->args[0] = strdup(req->builder); /* argv[0] = builder */
  for (int i = 0; i < req->argc; i++) {
    req->args[i + 1] = read_string(&ptr, end);
    if (!req->args[i + 1]) {
      return -1;
    }
  }
  req->argc++; /* Include builder in count */

  /* Environment */
  if (ptr + 4 > end) {
    return -1;
  }
  req->envc = ptr[0] | (ptr[1] << 8) | (ptr[2] << 16) | (ptr[3] << 24);
  ptr += 4;
  req->env = calloc(req->envc + 1, sizeof(char*)); /* +1 for NULL */
  if (!req->env) {
    return -1;
  }
  for (int i = 0; i < req->envc; i++) {
    char* key = read_string(&ptr, end);
    char* value = read_string(&ptr, end);
    if (!key || !value) {
      free(key);
      free(value);
      return -1;
    }
    /* Format as "KEY=value" */
    size_t klen = strlen(key);
    size_t vlen = strlen(value);
    req->env[i] = malloc(klen + 1 + vlen + 1);
    if (!req->env[i]) {
      free(key);
      free(value);
      return -1;
    }
    memcpy(req->env[i], key, klen);
    req->env[i][klen] = '=';
    memcpy(req->env[i] + klen + 1, value, vlen + 1);
    free(key);
    free(value);
  }

  /* Workdir */
  req->workdir = read_string(&ptr, end);
  if (!req->workdir) {
    return -1;
  }

  /* Outputs */
  if (ptr + 4 > end) {
    return -1;
  }
  req->output_count = ptr[0] | (ptr[1] << 8) | (ptr[2] << 16) | (ptr[3] << 24);
  ptr += 4;
  req->outputs = calloc(req->output_count, sizeof(char*));
  if (!req->outputs) {
    return -1;
  }
  for (int i = 0; i < req->output_count; i++) {
    req->outputs[i] = read_string(&ptr, end);
    if (!req->outputs[i]) {
      return -1;
    }
  }

  /* Extra files (optional, for backwards compatibility) */
  req->extra_files = NULL;
  req->extra_file_count = 0;
  if (ptr + 4 <= end) {
    req->extra_file_count = ptr[0] | (ptr[1] << 8) | (ptr[2] << 16) | (ptr[3] << 24);
    ptr += 4;
    if (req->extra_file_count > 0) {
      req->extra_files = calloc(req->extra_file_count, sizeof(struct extra_file));
      if (!req->extra_files) {
        return -1;
      }
      for (int i = 0; i < req->extra_file_count; i++) {
        req->extra_files[i].filename = read_string(&ptr, end);
        if (!req->extra_files[i].filename) {
          return -1;
        }
        req->extra_files[i].contents = read_blob(&ptr, end, &req->extra_files[i].contents_len);
        if (!req->extra_files[i].contents) {
          return -1;
        }
      }
    }
  }

  return 0;
}

/* ============================================================================
 * Build Execution
 * ============================================================================ */

/* Pipe output handler - reads from pipe and sends to host */
static void* output_thread(void* arg) {
  int* fds = (int*)arg;
  int read_fd = fds[0];
  uint16_t msg_type = (uint16_t)fds[1];

  char buf[4096];
  ssize_t n;

  while ((n = read(read_fd, buf, sizeof(buf))) > 0) {
    send_message(msg_type, buf, (uint32_t)n);
  }

  return NULL;
}
static int execute_builder(struct build_request* req) {
  int stdout_pipe[2], stderr_pipe[2];

  if (pipe(stdout_pipe) < 0 || pipe(stderr_pipe) < 0) {
    log_error("pipe() failed: %s", strerror(errno));
    return -1;
  }

  log_info("executing builder: %s", req->builder);

  /* Create build working directory */
  mkdir(req->workdir, 0755);

  /* Write extra files to workdir (passAsFile, structuredAttrs, etc.) */
  for (int i = 0; i < req->extra_file_count; i++) {
    char filepath[PATH_MAX];
    snprintf(filepath, sizeof(filepath), "%s/%s", req->workdir, req->extra_files[i].filename);

    int fd = open(filepath, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) {
      log_error("failed to create extra file %s: %s", filepath, strerror(errno));
      return -1;
    }

    size_t written = 0;
    while (written < req->extra_files[i].contents_len) {
      ssize_t n = write(fd, req->extra_files[i].contents + written,
                        req->extra_files[i].contents_len - written);
      if (n < 0) {
        if (errno == EINTR) {
          continue;
        }
        log_error("failed to write extra file %s: %s", filepath, strerror(errno));
        close(fd);
        return -1;
      }
      written += n;
    }
    close(fd);
    log_info("wrote extra file: %s (%zu bytes)", req->extra_files[i].filename,
             req->extra_files[i].contents_len);
  }

  pid_t pid = fork();
  if (pid < 0) {
    log_error("fork() failed: %s", strerror(errno));
    return -1;
  }

  if (pid == 0) {
    /* Child process */

    /* Create new session */
    setsid();

    /* Set up file descriptors */
    close(stdout_pipe[0]);
    close(stderr_pipe[0]);

    /* stdin from /dev/null */
    int null_fd = open("/dev/null", O_RDONLY);
    if (null_fd >= 0) {
      dup2(null_fd, STDIN_FILENO);
      close(null_fd);
    }

    /* stdout/stderr to pipes */
    dup2(stdout_pipe[1], STDOUT_FILENO);
    dup2(stderr_pipe[1], STDERR_FILENO);
    close(stdout_pipe[1]);
    close(stderr_pipe[1]);

    /* Change to build directory */
    if (chdir(req->workdir) < 0) {
      fprintf(stderr, "chdir(%s) failed: %s\n", req->workdir, strerror(errno));
      _exit(1);
    }

    /* Clear environment and set build environment */
    clearenv();
    for (int i = 0; req->env[i]; i++) {
      putenv(req->env[i]);
    }

    /* Execute builder */
    execv(req->builder, req->args);

    /* If we get here, execv failed */
    fprintf(stderr, "execv(%s) failed: %s\n", req->builder, strerror(errno));
    _exit(127);
  }

  /* Parent process */
  close(stdout_pipe[1]);
  close(stderr_pipe[1]);

  /* Read output from both pipes and send to host */
  fd_set readfds;
  int maxfd = (stdout_pipe[0] > stderr_pipe[0]) ? stdout_pipe[0] : stderr_pipe[0];
  bool stdout_open = true, stderr_open = true;
  char buf[4096];

  while (stdout_open || stderr_open) {
    FD_ZERO(&readfds);
    if (stdout_open) {
      FD_SET(stdout_pipe[0], &readfds);
    }
    if (stderr_open) {
      FD_SET(stderr_pipe[0], &readfds);
    }

    struct timeval tv = {.tv_sec = 1, .tv_usec = 0};
    int ret = select(maxfd + 1, &readfds, NULL, NULL, &tv);
    if (ret < 0) {
      if (errno == EINTR) {
        continue;
      }
      break;
    }
    if (ret == 0) {
      continue;
    }

    if (stdout_open && FD_ISSET(stdout_pipe[0], &readfds)) {
      ssize_t n = read(stdout_pipe[0], buf, sizeof(buf));
      if (n > 0) {
        send_message(MSG_BUILD_STDOUT, buf, (uint32_t)n);
      } else {
        stdout_open = false;
      }
    }

    if (stderr_open && FD_ISSET(stderr_pipe[0], &readfds)) {
      ssize_t n = read(stderr_pipe[0], buf, sizeof(buf));
      if (n > 0) {
        send_message(MSG_BUILD_STDERR, buf, (uint32_t)n);
      } else {
        stderr_open = false;
      }
    }
  }

  close(stdout_pipe[0]);
  close(stderr_pipe[0]);

  /* Wait for child */
  int status = 0;
  if (waitpid(pid, &status, 0) < 0) {
    log_error("waitpid failed: %s", strerror(errno));
    return -1;
  }

  int exit_code;
  if (WIFEXITED(status)) {
    exit_code = WEXITSTATUS(status);
    log_info("builder exited with code %d", exit_code);
  } else if (WIFSIGNALED(status)) {
    int sig = WTERMSIG(status);
    log_error("builder killed by signal %d", sig);
    exit_code = -sig;
  } else {
    log_error("builder exited with unknown status 0x%x", status);
    exit_code = -1;
  }

  return exit_code;
}

/* Send BUILD_EXIT response */
static void send_build_exit(int32_t exit_code, bool success, const char* error_msg) {
  size_t msg_len = error_msg ? strlen(error_msg) : 0;
  size_t payload_len = 4 + 4 + 4 + msg_len; /* exit_code + success+pad + len + msg */

  uint8_t* payload = malloc(payload_len);
  if (!payload) {
    return;
  }

  uint8_t* ptr = payload;

  /* exit_code (i32 as u32) */
  ptr[0] = exit_code & 0xff;
  ptr[1] = (exit_code >> 8) & 0xff;
  ptr[2] = (exit_code >> 16) & 0xff;
  ptr[3] = (exit_code >> 24) & 0xff;
  ptr += 4;

  /* success (u8) + padding (3 bytes) */
  ptr[0] = success ? 1 : 0;
  ptr[1] = 0;
  ptr[2] = 0;
  ptr[3] = 0;
  ptr += 4;

  /* error message length + data */
  ptr[0] = msg_len & 0xff;
  ptr[1] = (msg_len >> 8) & 0xff;
  ptr[2] = (msg_len >> 16) & 0xff;
  ptr[3] = (msg_len >> 24) & 0xff;
  ptr += 4;

  if (msg_len > 0) {
    memcpy(ptr, error_msg, msg_len);
  }

  send_message(MSG_BUILD_EXIT, payload, (uint32_t)payload_len);
  free(payload);
}

/* ============================================================================
 * Main Loop
 * ============================================================================ */

static volatile bool running = true;

static void signal_handler(int sig) {
  (void)sig;
  running = false;
}

static void handle_message(struct wire_header* hdr, uint8_t* payload) {
  switch (hdr->msg_type) {
    case MSG_BUILD_EXEC: {
      struct build_request req;
      if (parse_build_request(payload, hdr->payload_len, &req) < 0) {
        log_error("failed to parse BUILD_EXEC");
        send_build_exit(-1, false, "Invalid build request");
        break;
      }

      int exit_code = execute_builder(&req);
      send_build_exit(exit_code, exit_code == 0, NULL);
      free_build_request(&req);
      break;
    }

    case MSG_BUILD_ABORT:
      log_info("received BUILD_ABORT");
      /* TODO: Kill running builder */
      break;

    case MSG_PING:
      send_message(MSG_PONG, NULL, 0);
      break;

    default:
      log_error("unknown message type: 0x%04x", hdr->msg_type);
  }
}

static void main_loop(void) {
  struct wire_header hdr;
  uint8_t* payload = NULL;
  size_t payload_cap = 0;

  while (running) {
    if (recv_header(&hdr) < 0) {
      log_error("connection lost");
      break;
    }

    /* Reallocate payload buffer if needed */
    if (hdr.payload_len > payload_cap) {
      free(payload);
      payload_cap = hdr.payload_len + 4096;
      payload = malloc(payload_cap);
      if (!payload) {
        log_error("malloc failed");
        break;
      }
    }

    if (hdr.payload_len > 0) {
      if (read_exact(payload, hdr.payload_len) < 0) {
        log_error("failed to read payload");
        break;
      }
    }

    handle_message(&hdr, payload);
  }

  free(payload);
}

/* ============================================================================
 * Entry Point
 * ============================================================================ */

int main(int argc, char* argv[]) {
  (void)argc;
  (void)argv;

  log_info("starting (pid=%d)", getpid());

  /* Set up signal handlers */
  signal(SIGTERM, signal_handler);
  signal(SIGINT, signal_handler);

  /* NOTE: We do NOT set SIGCHLD to SIG_IGN because that causes the kernel
   * to auto-reap children, which breaks our explicit waitpid() calls.
   * Zombies shouldn't be an issue since we only spawn one child at a time
   * and always wait for it. */

  /* Set up filesystem */
  setup_filesystem();

  /* Listen for connection from host */
  if (vsock_listen_and_accept() < 0) {
    log_error("failed to accept connection from host");
    return 1;
  }

  /* Run main loop */
  main_loop();

  log_info("shutting down");

  if (vsock_fd >= 0) {
    close(vsock_fd);
  }

  /* Halt the VM */
  sync();
  reboot(0x4321fedc); /* LINUX_REBOOT_CMD_POWER_OFF */

  return 0;
}
