/*
 * test_protocol.c - Unit tests for nix-builder-init protocol handling
 *
 * These tests verify the wire protocol parsing without needing a VM.
 * Build and run on host:
 *   gcc -o test_protocol test_protocol.c -DTEST_PROTOCOL
 *   ./test_protocol
 */

#ifdef TEST_PROTOCOL

#  include <assert.h>
#  include <stdint.h>
#  include <stdio.h>
#  include <stdlib.h>
#  include <string.h>

/* ============================================================================
 * Wire Protocol Constants (must match vm_protocol.h)
 * ============================================================================ */

#  define VM_PROTOCOL_MAGIC 0x4E495842 /* "NIXB" */
#  define VM_PROTOCOL_VERSION 1

#  define MSG_BUILD_EXEC 0x0001
#  define MSG_BUILD_ABORT 0x0002
#  define MSG_PING 0x0003
#  define MSG_BUILD_STDOUT 0x0101
#  define MSG_BUILD_STDERR 0x0102
#  define MSG_BUILD_EXIT 0x0103
#  define MSG_PONG 0x0104
#  define MSG_WITNESS_EVENT 0x0105

/* Wire header - 12 bytes */
struct wire_header {
  uint32_t magic;
  uint16_t version;
  uint16_t msg_type;
  uint32_t payload_len;
} __attribute__((packed));

/* ============================================================================
 * Message Parsing (copied from nix-builder-init.c)
 * ============================================================================ */

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

struct build_request {
  char* builder;
  char** args;
  int argc;
  char** env;
  int envc;
  char* workdir;
  char** outputs;
  int output_count;
};

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
  req->args = calloc(req->argc + 2, sizeof(char*));
  if (!req->args) {
    return -1;
  }
  req->args[0] = strdup(req->builder);
  for (int i = 0; i < req->argc; i++) {
    req->args[i + 1] = read_string(&ptr, end);
    if (!req->args[i + 1]) {
      return -1;
    }
  }
  req->argc++;

  /* Environment */
  if (ptr + 4 > end) {
    return -1;
  }
  req->envc = ptr[0] | (ptr[1] << 8) | (ptr[2] << 16) | (ptr[3] << 24);
  ptr += 4;
  req->env = calloc(req->envc + 1, sizeof(char*));
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

  return 0;
}

static void free_build_request(struct build_request* req) {
  free(req->builder);
  for (int i = 0; i < req->argc; i++) {
    free(req->args[i]);
  }
  free(req->args);
  for (int i = 0; i < req->envc; i++) {
    free(req->env[i]);
  }
  free(req->env);
  free(req->workdir);
  for (int i = 0; i < req->output_count; i++) {
    free(req->outputs[i]);
  }
  free(req->outputs);
}

/* ============================================================================
 * Helper to build test payloads
 * ============================================================================ */

static void write_u32(uint8_t** ptr, uint32_t val) {
  (*ptr)[0] = val & 0xff;
  (*ptr)[1] = (val >> 8) & 0xff;
  (*ptr)[2] = (val >> 16) & 0xff;
  (*ptr)[3] = (val >> 24) & 0xff;
  *ptr += 4;
}

static void write_string(uint8_t** ptr, const char* str) {
  uint32_t len = strlen(str);
  write_u32(ptr, len);
  memcpy(*ptr, str, len);
  *ptr += len;
}

/* ============================================================================
 * Tests
 * ============================================================================ */

static void test_header_size(void) {
  printf("test_header_size: ");
  assert(sizeof(struct wire_header) == 12);
  printf("PASS\n");
}

static void test_parse_simple_request(void) {
  printf("test_parse_simple_request: ");

  /* Build payload:
   * builder: "/nix/store/xxx-bash/bin/bash"
   * args: ["-e", "script.sh"]
   * env: [("PATH", "/bin"), ("HOME", "/tmp")]
   * workdir: "/build"
   * outputs: ["/nix/store/yyy-hello"]
   */
  uint8_t payload[1024];
  uint8_t* ptr = payload;

  /* builder */
  write_string(&ptr, "/nix/store/xxx-bash/bin/bash");

  /* args count + args (excluding argv[0]) */
  write_u32(&ptr, 2); /* 2 args */
  write_string(&ptr, "-e");
  write_string(&ptr, "script.sh");

  /* env count + env pairs */
  write_u32(&ptr, 2); /* 2 env vars */
  write_string(&ptr, "PATH");
  write_string(&ptr, "/bin");
  write_string(&ptr, "HOME");
  write_string(&ptr, "/tmp");

  /* workdir */
  write_string(&ptr, "/build");

  /* outputs */
  write_u32(&ptr, 1);
  write_string(&ptr, "/nix/store/yyy-hello");

  size_t payload_len = ptr - payload;

  /* Parse */
  struct build_request req;
  int result = parse_build_request(payload, payload_len, &req);
  assert(result == 0);

  /* Verify */
  assert(strcmp(req.builder, "/nix/store/xxx-bash/bin/bash") == 0);
  assert(req.argc == 3); /* builder + 2 args */
  assert(strcmp(req.args[0], "/nix/store/xxx-bash/bin/bash") == 0);
  assert(strcmp(req.args[1], "-e") == 0);
  assert(strcmp(req.args[2], "script.sh") == 0);
  assert(req.args[3] == NULL);

  assert(req.envc == 2);
  assert(strcmp(req.env[0], "PATH=/bin") == 0);
  assert(strcmp(req.env[1], "HOME=/tmp") == 0);
  assert(req.env[2] == NULL);

  assert(strcmp(req.workdir, "/build") == 0);

  assert(req.output_count == 1);
  assert(strcmp(req.outputs[0], "/nix/store/yyy-hello") == 0);

  free_build_request(&req);
  printf("PASS\n");
}

static void test_parse_empty_args(void) {
  printf("test_parse_empty_args: ");

  uint8_t payload[1024];
  uint8_t* ptr = payload;

  write_string(&ptr, "/bin/true");
  write_u32(&ptr, 0); /* no args */
  write_u32(&ptr, 0); /* no env */
  write_string(&ptr, "/");
  write_u32(&ptr, 0); /* no outputs */

  size_t payload_len = ptr - payload;

  struct build_request req;
  int result = parse_build_request(payload, payload_len, &req);
  assert(result == 0);

  assert(strcmp(req.builder, "/bin/true") == 0);
  assert(req.argc == 1); /* just builder */
  assert(req.envc == 0);
  assert(req.output_count == 0);

  free_build_request(&req);
  printf("PASS\n");
}

static void test_truncated_payload(void) {
  printf("test_truncated_payload: ");

  /* Payload that claims longer string than available */
  uint8_t payload[8];
  payload[0] = 0xff; /* length = 255 */
  payload[1] = 0x00;
  payload[2] = 0x00;
  payload[3] = 0x00;
  payload[4] = 'h'; /* only 1 byte of data */

  struct build_request req;
  int result = parse_build_request(payload, 5, &req);
  assert(result == -1); /* should fail */

  printf("PASS\n");
}

static void test_wire_header_magic(void) {
  printf("test_wire_header_magic: ");

  struct wire_header hdr = {
      .magic = VM_PROTOCOL_MAGIC,
      .version = VM_PROTOCOL_VERSION,
      .msg_type = MSG_PING,
      .payload_len = 0,
  };

  uint8_t* bytes = (uint8_t*)&hdr;

  /* Check magic is "NIXB" in little-endian */
  assert(bytes[0] == 'B');
  assert(bytes[1] == 'X');
  assert(bytes[2] == 'I');
  assert(bytes[3] == 'N');

  printf("PASS\n");
}

int main(void) {
  printf("nix-builder-init protocol tests\n");
  printf("================================\n\n");

  test_header_size();
  test_parse_simple_request();
  test_parse_empty_args();
  test_truncated_payload();
  test_wire_header_magic();

  printf("\nAll tests passed!\n");
  return 0;
}

#endif /* TEST_PROTOCOL */
