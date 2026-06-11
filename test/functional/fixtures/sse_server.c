// Test-only HTTP/1.1 fixture with a scripted wire personality, used by the
// functional stream specs to exercise the curl_multi<->libuv bridge with
// byte-exact control over chunk boundaries, delays, and connection fate.
//
//   mua_sse_server --script PATH --capture PATH [--timeout-ms N]
//
// The script is a sequence of connection blocks (see test/functional/
// helpers.lua, which generates it):
//
//   script      := block ( "next\n" block )*
//   block       := step* ( "close\n" | "reset\n" )
//   step        := "send " N "\n" <exactly N raw bytes>  |  "sleep " MS "\n"
//
// One block serves one accepted connection: the full request (headers +
// Content-Length body) is read and appended to the capture file as a
// "REQUEST <index> <len>\n<raw>\n" record BEFORE any directive runs. `send`
// payloads are the raw response bytes (status line included): the server has
// zero response logic, so responses must carry "Connection: close" and use
// close-delimited framing. `close` ends with a clean FIN; `reset` ends with
// an RST (SO_LINGER{1,0}). Expect/100-continue is deliberately unsupported:
// keep request bodies small (curl only sends Expect for bodies over ~1 KiB).
//
// Exit codes: 0 script fully executed; 2 watchdog timeout; 3 protocol
// violation (unexpected connection, request over caps, client died early,
// write failure); 64 usage or script-validation error (before PORT prints).
//
// Stdout carries exactly one line, "PORT <n>\n". Diagnostics go to stderr
// directly (never through log.h: a failing fixture must not hide behind
// MUA_LOG).

#include <arpa/inet.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>

#include <uv.h>

#include "mua/memory.h"

enum {
  kMaxScriptBytes = 1024 * 1024,
  kMaxDirectives = 4096,
  kMaxSendBytes = 256 * 1024,
  kMaxSleepMs = 60000,
  kMaxBlocks = 16, // provider retry cap is 5; generous headroom
  kMaxHeaderBytes = 32 * 1024,
  kMaxBodyBytes = 1024 * 1024,
  kDefaultTimeoutMs = 30000,
  kListenBacklog = 8,
  kReadBufSize = 64 * 1024,
};

enum {
  kExitOk = 0,
  kExitWatchdog = 2,
  kExitViolation = 3,
  kExitUsage = 64,
};

typedef enum {
  kDirSend = 0,
  kDirSleep,
  kDirClose,
  kDirReset,
  kDirNext,
} DirectiveKind;

typedef struct {
  DirectiveKind kind;
  const char *bytes; // kDirSend: slice into the script blob
  size_t len;
  uint64_t sleep_ms;
} Directive;

typedef enum {
  kStateAwaitConn = 0,
  kStateReadRequest,
  kStateScripting,
} ServerState;

// The whole fixture is one connection at a time by design; a single static
// instance is the documented state of this single-purpose test binary.
static struct {
  uv_loop_t *loop;
  uv_tcp_t listener;
  uv_timer_t watchdog;
  uv_timer_t sleep_timer;
  uv_tcp_t conn;
  uv_write_t write_req;
  char *script_blob;
  size_t script_size;
  Directive directives[kMaxDirectives];
  size_t directive_count;
  size_t cursor;
  ServerState state;
  int connections;
  Buf request;
  char read_buf[kReadBufSize];
  size_t header_end; // offset just past the blank line; 0 while unseen
  size_t content_length;
  FILE *capture;
  bool reset_on_close;
} g_server;

static void die_usage(const char *what)
{
  // Pre-PORT failure path; best-effort diagnostics.
  (void)fprintf(stderr, "mua_sse_server: %s\n", what);
  exit(kExitUsage);
}

static void die_violation(const char *what)
{
  (void)fprintf(stderr, "mua_sse_server: protocol violation: %s\n", what);
  exit(kExitViolation);
}

static bool parse_decimal(const char *str, size_t len, uint64_t *out)
{
  if (len == 0 || len > 9) {
    return false;
  }
  uint64_t value = 0;
  for (size_t i = 0; i < len; i++) { // bounded by 9
    if (str[i] < '0' || str[i] > '9') {
      return false;
    }
    value = (value * 10) + (uint64_t)(str[i] - '0');
  }
  *out = value;
  return true;
}

// Parses one directive at `pos`; returns the offset just past it.
static size_t parse_directive(const char *blob, size_t size, size_t pos, Directive *out)
{
  const char *line = blob + pos;
  size_t remaining = size - pos;
  const char *newline = memchr(line, '\n', remaining);
  if (newline == NULL) {
    die_usage("script: directive line without newline");
  }
  size_t line_len = (size_t)(newline - line);
  size_t next = pos + line_len + 1;
  if (line_len >= 5 && memcmp(line, "send ", 5) == 0) {
    uint64_t count = 0;
    if (!parse_decimal(line + 5, line_len - 5, &count) || count == 0 || count > kMaxSendBytes) {
      die_usage("script: bad send count");
    }
    if (count > size - next) {
      die_usage("script: send payload truncated");
    }
    *out = (Directive){.kind = kDirSend, .bytes = blob + next, .len = (size_t)count};
    return next + (size_t)count;
  }
  if (line_len >= 6 && memcmp(line, "sleep ", 6) == 0) {
    uint64_t ms = 0;
    if (!parse_decimal(line + 6, line_len - 6, &ms) || ms > kMaxSleepMs) {
      die_usage("script: bad sleep duration");
    }
    *out = (Directive){.kind = kDirSleep, .sleep_ms = ms};
    return next;
  }
  if (line_len == 5 && memcmp(line, "close", 5) == 0) {
    *out = (Directive){.kind = kDirClose};
    return next;
  }
  if (line_len == 5 && memcmp(line, "reset", 5) == 0) {
    *out = (Directive){.kind = kDirReset};
    return next;
  }
  if (line_len == 4 && memcmp(line, "next", 4) == 0) {
    *out = (Directive){.kind = kDirNext};
    return next;
  }
  die_usage("script: unknown directive");
  return size; // unreachable; die_usage exits
}

// Structural validation: every block ends in a terminator, `next` separates
// blocks (never trailing), block count within the cap.
static void validate_script(void)
{
  bool in_block = true; // a script starts inside its first block
  int blocks = 1;
  if (g_server.directive_count == 0) {
    die_usage("script: empty");
  }
  for (size_t i = 0; i < g_server.directive_count; i++) {
    DirectiveKind kind = g_server.directives[i].kind;
    if (kind == kDirNext) {
      if (in_block) {
        die_usage("script: next before the block terminator");
      }
      if (i + 1 >= g_server.directive_count) {
        die_usage("script: next without a following block");
      }
      blocks++;
      if (blocks > kMaxBlocks) {
        die_usage("script: too many connection blocks");
      }
      in_block = true;
    } else if (kind == kDirClose || kind == kDirReset) {
      if (!in_block) {
        die_usage("script: terminator outside a block");
      }
      in_block = false;
    } else if (!in_block) {
      die_usage("script: directive after the block terminator");
    }
  }
  if (in_block) {
    die_usage("script: final block lacks a close/reset terminator");
  }
}

static void load_script(const char *path)
{
  FILE *file = fopen(path, "rb");
  if (file == NULL) {
    die_usage("cannot open the script file");
  }
  char *blob = xmalloc(kMaxScriptBytes + 1);
  size_t size = fread(blob, 1, kMaxScriptBytes + 1, file);
  if (ferror(file) != 0) {
    die_usage("script read failed");
  }
  if (fclose(file) != 0) {
    die_usage("script close failed");
  }
  if (size > kMaxScriptBytes) {
    die_usage("script exceeds the size cap");
  }
  g_server.script_blob = blob;
  g_server.script_size = size;
  size_t pos = 0;
  while (pos < size) { // bounded: parse_directive strictly advances pos
    if (g_server.directive_count >= kMaxDirectives) {
      die_usage("script: too many directives");
    }
    pos = parse_directive(blob, size, pos, &g_server.directives[g_server.directive_count]);
    g_server.directive_count++;
  }
  validate_script();
}

static void write_capture_record(void)
{
  if (fprintf(g_server.capture, "REQUEST %d %zu\n", g_server.connections, g_server.request.size) <
      0) {
    die_violation("capture write failed");
  }
  if (g_server.request.size > 0 && fwrite(g_server.request.data, 1, g_server.request.size,
                                          g_server.capture) != g_server.request.size) {
    die_violation("capture write failed");
  }
  if (fputc('\n', g_server.capture) == EOF || fflush(g_server.capture) != 0) {
    die_violation("capture flush failed");
  }
}

static size_t find_header_end(const char *data, size_t size)
{
  if (size < 4) {
    return 0;
  }
  // Bounded by the request buffer cap; rescanned per read of a <=32 KiB head.
  for (size_t i = 0; i + 3 < size; i++) {
    if (data[i] == '\r' && data[i + 1] == '\n' && data[i + 2] == '\r' && data[i + 3] == '\n') {
      return i + 4;
    }
  }
  return 0;
}

static char ascii_lower(char c)
{
  return (c >= 'A' && c <= 'Z') ? (char)(c + ('a' - 'A')) : c;
}

// Hand-rolled (not strncasecmp: strict -std=c11 hides POSIX declarations on
// some libcs, a future Linux-CI -Werror trap).
static bool line_is_content_length(const char *line, size_t len, size_t *value_out)
{
  static const char name[] = "content-length:";
  size_t name_len = sizeof(name) - 1;
  if (len <= name_len) {
    return false;
  }
  for (size_t i = 0; i < name_len; i++) {
    if (ascii_lower(line[i]) != name[i]) {
      return false;
    }
  }
  size_t pos = name_len;
  while (pos < len && line[pos] == ' ') { // bounded by line length
    pos++;
  }
  uint64_t value = 0;
  if (!parse_decimal(line + pos, len - pos, &value) || value > kMaxBodyBytes) {
    die_violation("unparseable or oversized Content-Length");
  }
  *value_out = (size_t)value;
  return true;
}

static void parse_content_length(void)
{
  const char *head = g_server.request.data;
  size_t head_len = g_server.header_end - 4; // up to (not including) the blank line
  size_t line_start = 0;
  g_server.content_length = 0;
  // Bounded by the header cap; each iteration consumes one header line.
  while (line_start < head_len) {
    const char *eol = memchr(head + line_start, '\r', head_len - line_start);
    size_t line_len = (eol != NULL) ? (size_t)(eol - (head + line_start)) : head_len - line_start;
    if (line_is_content_length(head + line_start, line_len, &g_server.content_length)) {
      return;
    }
    line_start += line_len + 2; // skip the CRLF
  }
}

static void script_step(void);

static void conn_closed_cb(uv_handle_t *handle)
{
  (void)handle;
  if (g_server.cursor >= g_server.directive_count) {
    // Script exhausted: orderly shutdown; uv_run drains and main returns 0.
    uv_close((uv_handle_t *)&g_server.watchdog, NULL);
    uv_close((uv_handle_t *)&g_server.sleep_timer, NULL);
    uv_close((uv_handle_t *)&g_server.listener, NULL);
    return;
  }
  if (g_server.directives[g_server.cursor].kind != kDirNext) {
    die_violation("internal: terminator not followed by next"); // validator prevents this
  }
  g_server.cursor++;
  g_server.state = kStateAwaitConn;
}

static void close_conn(void)
{
  if (g_server.reset_on_close) {
    uv_os_fd_t fd = -1;
    if (uv_fileno((uv_handle_t *)&g_server.conn, &fd) != 0) {
      die_violation("uv_fileno failed for reset");
    }
    struct linger lg = {.l_onoff = 1, .l_linger = 0};
    if (setsockopt(fd, SOL_SOCKET, SO_LINGER, &lg, sizeof(lg)) != 0) {
      die_violation("SO_LINGER failed for reset");
    }
  }
  uv_close((uv_handle_t *)&g_server.conn, conn_closed_cb);
}

static void write_done_cb(uv_write_t *req, int status)
{
  (void)req;
  if (status != 0) {
    die_violation("response write failed (client gone?)");
  }
  script_step();
}

static void sleep_done_cb(uv_timer_t *handle)
{
  (void)handle;
  script_step();
}

// The dispatcher: executes exactly one directive, then returns; re-entered
// only from loop-deferred callbacks (write-done, timer, close), so there is
// no recursion and progress is bounded by the directive count.
static void script_step(void)
{
  if (g_server.cursor >= g_server.directive_count) {
    die_violation("internal: script cursor overran"); // validator prevents this
  }
  Directive *dir = &g_server.directives[g_server.cursor];
  g_server.cursor++;
  switch (dir->kind) {
    case kDirSend: {
      uv_buf_t buf = uv_buf_init((char *)dir->bytes, (unsigned int)dir->len);
      if (uv_write(&g_server.write_req, (uv_stream_t *)&g_server.conn, &buf, 1, write_done_cb) !=
          0) {
        die_violation("uv_write failed");
      }
      return;
    }
    case kDirSleep:
      if (uv_timer_start(&g_server.sleep_timer, sleep_done_cb, dir->sleep_ms, 0) != 0) {
        die_violation("sleep timer failed");
      }
      return;
    case kDirClose:
      g_server.reset_on_close = false;
      close_conn();
      return;
    case kDirReset:
      g_server.reset_on_close = true;
      close_conn();
      return;
    case kDirNext:
      die_violation("internal: next reached mid-block"); // validator prevents this
      return;
  }
}

static void request_maybe_complete(void)
{
  if (g_server.header_end == 0) {
    g_server.header_end = find_header_end(g_server.request.data, g_server.request.size);
    if (g_server.header_end == 0) {
      if (g_server.request.size > kMaxHeaderBytes) {
        die_violation("request head exceeds the cap");
      }
      return;
    }
    parse_content_length();
  }
  if (g_server.request.size < g_server.header_end + g_server.content_length) {
    return; // body still arriving
  }
  if (uv_read_stop((uv_stream_t *)&g_server.conn) != 0) {
    die_violation("uv_read_stop failed");
  }
  write_capture_record();
  g_server.state = kStateScripting;
  script_step();
}

static void alloc_cb(uv_handle_t *handle, size_t suggested_size, uv_buf_t *buf)
{
  (void)handle;
  (void)suggested_size;
  buf->base = g_server.read_buf;
  buf->len = kReadBufSize;
}

static void read_cb(uv_stream_t *stream, ssize_t nread, const uv_buf_t *buf)
{
  (void)stream;
  (void)buf;
  if (nread == 0) {
    return; // spurious wakeup; not an error
  }
  if (nread < 0) {
    die_violation("client closed before sending a full request");
  }
  if (!buf_append(&g_server.request, g_server.read_buf, (size_t)nread)) {
    die_violation("request exceeds the caps");
  }
  request_maybe_complete();
}

static void on_connection(uv_stream_t *listener, int status)
{
  if (status != 0) {
    die_violation("listener error");
  }
  if (g_server.state != kStateAwaitConn) {
    die_violation("connection while a block is still in progress");
  }
  if (g_server.connections >= kMaxBlocks) {
    die_violation("more connections than scripted blocks");
  }
  if (uv_tcp_init(g_server.loop, &g_server.conn) != 0) {
    die_violation("uv_tcp_init failed");
  }
  if (uv_accept(listener, (uv_stream_t *)&g_server.conn) != 0) {
    die_violation("uv_accept failed");
  }
  // Best effort: nodelay only affects how faithfully scripted slices map to
  // segments, never correctness.
  (void)uv_tcp_nodelay(&g_server.conn, 1);
  g_server.connections++;
  buf_reset(&g_server.request);
  g_server.header_end = 0;
  g_server.content_length = 0;
  g_server.state = kStateReadRequest;
  if (uv_read_start((uv_stream_t *)&g_server.conn, alloc_cb, read_cb) != 0) {
    die_violation("uv_read_start failed");
  }
}

static void watchdog_cb(uv_timer_t *handle)
{
  (void)handle;
  (void)fprintf(stderr, "mua_sse_server: watchdog timeout; script not consumed\n");
  exit(kExitWatchdog);
}

static int serve(uint64_t timeout_ms)
{
  g_server.loop = uv_default_loop();
  if (g_server.loop == NULL) {
    die_usage("uv_default_loop failed");
  }
  if (uv_timer_init(g_server.loop, &g_server.watchdog) != 0 ||
      uv_timer_init(g_server.loop, &g_server.sleep_timer) != 0) {
    die_usage("timer init failed");
  }
  if (uv_timer_start(&g_server.watchdog, watchdog_cb, timeout_ms, 0) != 0) {
    die_usage("watchdog start failed");
  }
  struct sockaddr_in addr;
  if (uv_tcp_init(g_server.loop, &g_server.listener) != 0 ||
      uv_ip4_addr("127.0.0.1", 0, &addr) != 0 ||
      uv_tcp_bind(&g_server.listener, (const struct sockaddr *)&addr, 0) != 0 ||
      uv_listen((uv_stream_t *)&g_server.listener, kListenBacklog, on_connection) != 0) {
    die_usage("failed to bind/listen on 127.0.0.1");
  }
  struct sockaddr_storage bound;
  int bound_len = sizeof(bound);
  if (uv_tcp_getsockname(&g_server.listener, (struct sockaddr *)&bound, &bound_len) != 0) {
    die_usage("getsockname failed");
  }
  int port = ntohs(((struct sockaddr_in *)&bound)->sin_port);
  if (printf("PORT %d\n", port) < 0 || fflush(stdout) != 0) {
    die_usage("failed to report the port"); // the helper depends on this line
  }
  (void)uv_run(g_server.loop, UV_RUN_DEFAULT); // returns once every handle closed
  if (uv_loop_close(g_server.loop) != 0) {
    die_violation("event loop closed with live handles");
  }
  if (fclose(g_server.capture) != 0) {
    die_violation("capture close failed");
  }
  return kExitOk;
}

int main(int argc, char **argv)
{
  const char *script_path = NULL;
  const char *capture_path = NULL;
  uint64_t timeout_ms = kDefaultTimeoutMs;
  for (int i = 1; i < argc; i++) { // bounded by argc
    const char *arg = argv[i];
    const char *value = (i + 1 < argc) ? argv[i + 1] : NULL;
    if (strcmp(arg, "--script") == 0 && value != NULL) {
      script_path = value;
      i++;
    } else if (strcmp(arg, "--capture") == 0 && value != NULL) {
      capture_path = value;
      i++;
    } else if (strcmp(arg, "--timeout-ms") == 0 && value != NULL) {
      if (!parse_decimal(value, strlen(value), &timeout_ms) || timeout_ms == 0) {
        die_usage("bad --timeout-ms value");
      }
      i++;
    } else {
      die_usage("usage: mua_sse_server --script PATH --capture PATH [--timeout-ms N]");
    }
  }
  if (script_path == NULL || capture_path == NULL) {
    die_usage("usage: mua_sse_server --script PATH --capture PATH [--timeout-ms N]");
  }
  if (signal(SIGPIPE, SIG_IGN) == SIG_ERR) {
    die_usage("failed to ignore SIGPIPE");
  }
  load_script(script_path);
  g_server.capture = fopen(capture_path, "wb");
  if (g_server.capture == NULL) {
    die_usage("cannot open the capture file");
  }
  buf_init(&g_server.request, kMaxHeaderBytes + kMaxBodyBytes);
  return serve(timeout_ms);
}
