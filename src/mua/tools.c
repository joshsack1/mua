#include "mua/tools.h"

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include <uv.h>

#include "mua/api/private/helpers.h"
#include "mua/json.h"
#include "mua/log.h"
#include "mua/loop.h"
#include "mua/memory.h"
#include "mua/paths.h"

enum {
  kToolReadReturnCap = 256 * 1024,    // window bytes handed back to the model
  kToolReadScanCap = 8 * 1024 * 1024, // bytes examined on disk per call
  kToolReadChunk = 64 * 1024,
  kToolEditFileCap = 4 * 1024 * 1024, // whole-file rewrite bound
  kToolEditCountCap = 100,            // occurrence counting stops here
  kToolBashOutputCap = 64 * 1024,     // merged stdout+stderr kept for the model
  kToolBashScratch = 8 * 1024,        // read_cb scratch buffer
  kToolBashDefaultTimeoutMs = 30000,
  kToolBashMaxTimeoutMs = 120000, // hard ceiling regardless of the model's value
  kToolBashGraceMs = 500,         // post-exit wait for orphaned pipe holders
  kToolSchemaCap = 16 * 1024,     // parse bound for the static builtin schemas
  kToolEintrMax = 100,            // consecutive EINTR retries
};

static char *vformat(const char *fmt, va_list ap)
{
  va_list ap2;
  va_copy(ap2, ap);
  int need = vsnprintf(NULL, 0, fmt, ap);
  if (need < 0) {
    need = 0; // a formatting failure degrades to an empty message, not UB
  }
  char *msg = xmalloc((size_t)need + 1);
  msg[0] = '\0';
  (void)vsnprintf(msg, (size_t)need + 1, fmt, ap2); // sized by the first pass
  va_end(ap2);
  return msg;
}

// Fire a printf-formatted failure result; the model sees this text verbatim.
static void fail(ToolDoneCb done, void *ud, const char *fmt, ...) MUA_PRINTF_ATTR(3, 4);

static void fail(ToolDoneCb done, void *ud, const char *fmt, ...)
{
  va_list ap;
  va_start(ap, fmt);
  char *msg = vformat(fmt, ap);
  va_end(ap);
  ToolResult result = {.content = msg, .is_error = true};
  done(ud, &result);
}

// Fire a printf-formatted success result. Never route file content through
// the format string; succeed() carries arbitrary bytes.
static void okf(ToolDoneCb done, void *ud, const char *fmt, ...) MUA_PRINTF_ATTR(3, 4);

static void okf(ToolDoneCb done, void *ud, const char *fmt, ...)
{
  va_list ap;
  va_start(ap, fmt);
  char *msg = vformat(fmt, ap);
  va_end(ap);
  ToolResult result = {.content = msg, .is_error = false};
  done(ud, &result);
}

static void succeed(ToolDoneCb done, void *ud, char *content) // takes ownership
{
  ToolResult result = {.content = content, .is_error = false};
  done(ud, &result);
}

// Optional positive-integer argument: absent -> fallback (true); present but
// non-numeric or < 1 -> false.
static bool opt_pos_int(const cJSON *args, const char *key, int64_t fallback, int64_t *out)
{
  const cJSON *item = cJSON_GetObjectItemCaseSensitive(args, key);
  if (item == NULL) {
    *out = fallback;
    return true;
  }
  if (!cJSON_IsNumber(item)) {
    return false;
  }
  int64_t value = (int64_t)cJSON_GetNumberValue(item);
  if (value < 1) {
    return false;
  }
  *out = value;
  return true;
}

typedef struct {
  int64_t offset;       // first line wanted, 1-based
  int64_t limit;        // max lines returned
  int64_t current_line; // line number of the byte being examined
  int64_t lines_taken;  // complete lines already in the window
  size_t scanned;       // total bytes examined
  Buf window;
  bool window_full; // return cap hit: head-truncated
  bool finished;    // limit satisfied or window full; stop reading
} ReadScan;

// Walks one chunk by newline segments, appending in-window bytes. Bounded by
// the chunk length.
static void read_scan_chunk(ReadScan *scan, const char *chunk, size_t len)
{
  size_t pos = 0;
  while (pos < len && !scan->finished) {
    const char *nl = memchr(chunk + pos, '\n', len - pos);
    size_t seg_end = (nl != NULL) ? (size_t)(nl - chunk) + 1 : len; // include the '\n'
    if (scan->current_line >= scan->offset) {
      size_t seg_len = seg_end - pos;
      if (!buf_append(&scan->window, chunk + pos, seg_len)) {
        // Head-truncate: keep exactly the bytes that fit.
        size_t room = scan->window.max - scan->window.size;
        if (room > 0) {
          (void)buf_append(&scan->window, chunk + pos, room); // exact fit cannot refuse
        }
        scan->window_full = true;
        scan->finished = true;
        return;
      }
      if (nl != NULL) {
        scan->lines_taken++;
        if (scan->lines_taken >= scan->limit) {
          scan->finished = true;
        }
      }
    }
    if (nl != NULL) {
      scan->current_line++;
    }
    pos = seg_end;
  }
}

typedef enum {
  kReadOk = 0,
  kReadCapped, // scan bound hit before the window completed
  kReadBinary,
  kReadIoError,
} ReadStatus;

static ReadStatus read_scan_file(int fd, ReadScan *scan, int *io_errno)
{
  char *chunk = xmalloc(kToolReadChunk);
  ReadStatus status = kReadOk;
  int eintr = 0;
  while (!scan->finished) {
    if (scan->scanned >= kToolReadScanCap) {
      status = kReadCapped;
      break;
    }
    size_t want = kToolReadChunk;
    if (kToolReadScanCap - scan->scanned < want) {
      want = kToolReadScanCap - scan->scanned;
    }
    ssize_t got = read(fd, chunk, want);
    if (got < 0) {
      if (errno == EINTR && ++eintr <= kToolEintrMax) {
        continue;
      }
      *io_errno = errno;
      status = kReadIoError;
      break;
    }
    if (got == 0) {
      break; // EOF
    }
    eintr = 0;
    scan->scanned += (size_t)got;
    if (memchr(chunk, '\0', (size_t)got) != NULL) {
      status = kReadBinary;
      break;
    }
    read_scan_chunk(scan, chunk, (size_t)got);
  }
  xfree(chunk);
  return status;
}

// The window bytes verbatim plus the applicable annotation, as one xmalloc'd
// C string.
static char *read_assemble_content(const ReadScan *scan, ReadStatus status)
{
  const char *note = "";
  if (scan->window_full) {
    note = "[read: truncated at 256 KiB]";
  } else if (status == kReadCapped) {
    note = "[read: stopped at the 8 MiB scan bound]";
  } else if (scan->window.size == 0 && scan->scanned > 0) {
    note = "[read: offset past end of file]";
  }
  size_t note_len = strlen(note);
  bool sep = scan->window.size > 0 && note_len > 0;
  char *content = xmalloc(scan->window.size + (sep ? 1 : 0) + note_len + 1);
  size_t pos = 0;
  if (scan->window.size > 0) {
    memcpy(content, scan->window.data, scan->window.size);
    pos = scan->window.size;
  }
  if (sep) {
    content[pos] = '\n';
    pos++;
  }
  memcpy(content + pos, note, note_len + 1); // includes the NUL
  return content;
}

static ToolExec *read_execute(cJSON *args, ToolDoneCb done, void *ud)
{
  const char *path = json_get_cstr(args, "path");
  if (path == NULL || path[0] == '\0') {
    fail(done, ud, "read: path is required");
    return NULL;
  }
  int64_t offset = 1;
  int64_t limit = INT64_MAX;
  if (!opt_pos_int(args, "offset", 1, &offset) || !opt_pos_int(args, "limit", INT64_MAX, &limit)) {
    fail(done, ud, "read: offset and limit must be integers >= 1");
    return NULL;
  }
  int fd = open(path, O_RDONLY | O_CLOEXEC);
  if (fd < 0) {
    fail(done, ud, "read: cannot open %s: %s", path, strerror(errno));
    return NULL;
  }
  struct stat st;
  if (fstat(fd, &st) != 0) {
    fail(done, ud, "read: stat %s: %s", path, strerror(errno));
    (void)close(fd); // nothing useful to add if close also fails
    return NULL;
  }
  if (!S_ISREG(st.st_mode)) {
    fail(done, ud, "read: %s is not a regular file", path);
    (void)close(fd);
    return NULL;
  }
  ReadScan scan = {.offset = offset, .limit = limit, .current_line = 1};
  buf_init(&scan.window, kToolReadReturnCap);
  int io_errno = 0;
  ReadStatus status = read_scan_file(fd, &scan, &io_errno);
  (void)close(fd); // read-only stream; the data is already ours
  if (status == kReadBinary) {
    buf_free(&scan.window);
    fail(done, ud, "read: %s is binary", path);
    return NULL;
  }
  if (status == kReadIoError) {
    buf_free(&scan.window);
    fail(done, ud, "read: read %s: %s", path, strerror(io_errno));
    return NULL;
  }
  char *content = read_assemble_content(&scan, status);
  buf_free(&scan.window);
  succeed(done, ud, content);
  return NULL;
}

// Writes all `len` bytes; iterations are bounded by progress + the EINTR cap.
static bool write_all(int fd, const char *data, size_t len, int *io_errno)
{
  size_t written = 0;
  int eintr = 0;
  while (written < len) {
    ssize_t got = write(fd, data + written, len - written);
    if (got < 0) {
      if (errno == EINTR && ++eintr <= kToolEintrMax) {
        continue;
      }
      *io_errno = errno;
      return false;
    }
    if (got == 0) { // no progress and no error: refuse to spin
      *io_errno = EIO;
      return false;
    }
    eintr = 0;
    written += (size_t)got;
  }
  return true;
}

static ToolExec *write_execute(cJSON *args, ToolDoneCb done, void *ud)
{
  const char *path = json_get_cstr(args, "path");
  if (path == NULL || path[0] == '\0') {
    fail(done, ud, "write: path is required");
    return NULL;
  }
  const char *content = json_get_cstr(args, "content");
  if (content == NULL) { // "" is a valid write; missing/non-string is not
    fail(done, ud, "write: content is required");
    return NULL;
  }
  const char *last_slash = strrchr(path, '/');
  if (last_slash != NULL) { // no '/': plain filename, no parent to create
    char *parent =
      (last_slash == path) ? xstrdup("/") : xstrndup(path, (size_t)(last_slash - path));
    Error err = ERROR_INIT;
    bool dir_ok = paths_ensure_dir(parent, &err);
    xfree(parent);
    if (!dir_ok) {
      fail(done, ud, "write: %s", err.msg != NULL ? err.msg : "cannot create parent directory");
      api_clear_error(&err);
      return NULL;
    }
  }
  // 0666 & umask, like shell tools: these are user files, not private state.
  int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0666);
  if (fd < 0) {
    fail(done, ud, "write: cannot open %s: %s", path, strerror(errno));
    return NULL;
  }
  size_t len = strlen(content);
  int io_errno = 0;
  if (!write_all(fd, content, len, &io_errno)) {
    fail(done, ud, "write: write %s: %s", path, strerror(io_errno));
    (void)close(fd); // already failing; nothing useful to add
    return NULL;
  }
  if (close(fd) != 0) { // deferred I/O errors (quota, NFS) surface here
    fail(done, ud, "write: close %s: %s", path, strerror(errno));
    return NULL;
  }
  okf(done, ud, "write: wrote %zu bytes to %s", len, path);
  return NULL;
}

// Overlapping occurrence count (advance by one byte per hit), capped, with
// the first match position recorded. Explicit lengths throughout: the
// haystack may contain NUL bytes.
static int count_occurrences(const char *hay, size_t hay_len, const char *needle, size_t needle_len,
                             size_t *first_pos)
{
  int count = 0;
  if (needle_len == 0 || needle_len > hay_len) {
    return 0;
  }
  const char *cursor = hay;
  size_t left = hay_len;
  while (left >= needle_len) { // bounded by hay_len
    const char *hit = memchr(cursor, needle[0], left - needle_len + 1);
    if (hit == NULL) {
      return count;
    }
    left -= (size_t)(hit - cursor);
    cursor = hit;
    if (memcmp(cursor, needle, needle_len) == 0) {
      if (count == 0) {
        *first_pos = (size_t)(cursor - hay);
      }
      count++;
      if (count >= kToolEditCountCap) {
        return count;
      }
    }
    cursor++;
    left--;
  }
  return count;
}

// Opens the edit target and reads it whole. Returns the open fd (caller owns)
// or -1 with the failure result already fired.
static int edit_open_and_read(const char *path, char **data_out, size_t *len_out, ToolDoneCb done,
                              void *ud)
{
  int fd = open(path, O_RDWR | O_CLOEXEC);
  if (fd < 0) {
    fail(done, ud, "edit: cannot open %s: %s", path, strerror(errno));
    return -1;
  }
  struct stat st;
  if (fstat(fd, &st) != 0) {
    fail(done, ud, "edit: stat %s: %s", path, strerror(errno));
    (void)close(fd);
    return -1;
  }
  if (!S_ISREG(st.st_mode)) {
    fail(done, ud, "edit: %s is not a regular file", path);
    (void)close(fd);
    return -1;
  }
  if (st.st_size > kToolEditFileCap) {
    fail(done, ud, "edit: %s exceeds the 4 MiB edit cap", path);
    (void)close(fd);
    return -1;
  }
  size_t size = (size_t)st.st_size;
  char *data = xmalloc(size > 0 ? size : 1);
  size_t got_total = 0;
  int eintr = 0;
  while (got_total < size) { // bounded by the fstat size latch
    ssize_t got = read(fd, data + got_total, size - got_total);
    if (got < 0) {
      if (errno == EINTR && ++eintr <= kToolEintrMax) {
        continue;
      }
      fail(done, ud, "edit: read %s: %s", path, strerror(errno));
      xfree(data);
      (void)close(fd);
      return -1;
    }
    if (got == 0) {
      break; // shrank under us; edit what arrived
    }
    eintr = 0;
    got_total += (size_t)got;
  }
  *data_out = data;
  *len_out = got_total;
  return fd;
}

// The point of no return: shrink and rewrite on the same fd, preserving
// inode, hardlinks, owner, and symlink targets. Consumes fd; fires the
// failure result itself.
static bool edit_rewrite(int fd, const char *path, const char *data, size_t len, ToolDoneCb done,
                         void *ud)
{
  if (ftruncate(fd, 0) != 0) {
    fail(done, ud, "edit: truncate %s: %s", path, strerror(errno));
    (void)close(fd);
    return false;
  }
  if (lseek(fd, 0, SEEK_SET) == (off_t)-1) {
    fail(done, ud, "edit: seek %s: %s", path, strerror(errno));
    (void)close(fd);
    return false;
  }
  int io_errno = 0;
  if (!write_all(fd, data, len, &io_errno)) {
    fail(done, ud, "edit: write %s: %s", path, strerror(io_errno));
    (void)close(fd);
    return false;
  }
  if (close(fd) != 0) {
    fail(done, ud, "edit: close %s: %s", path, strerror(errno));
    return false;
  }
  return true;
}

static ToolExec *edit_execute(cJSON *args, ToolDoneCb done, void *ud)
{
  const char *path = json_get_cstr(args, "path");
  if (path == NULL || path[0] == '\0') {
    fail(done, ud, "edit: path is required");
    return NULL;
  }
  const char *old_str = json_get_cstr(args, "old_string");
  const char *new_str = json_get_cstr(args, "new_string");
  if (old_str == NULL || new_str == NULL) { // "" new_string is a deletion; missing is not
    fail(done, ud, "edit: old_string and new_string are required");
    return NULL;
  }
  if (old_str[0] == '\0') {
    fail(done, ud, "edit: old_string is empty");
    return NULL;
  }
  if (strcmp(old_str, new_str) == 0) {
    fail(done, ud, "edit: old_string and new_string are identical");
    return NULL;
  }
  char *file_data = NULL;
  size_t file_len = 0;
  int fd = edit_open_and_read(path, &file_data, &file_len, done, ud);
  if (fd < 0) {
    return NULL;
  }
  size_t old_len = strlen(old_str);
  size_t new_len = strlen(new_str);
  size_t first = 0;
  int count = count_occurrences(file_data, file_len, old_str, old_len, &first);
  if (count != 1) {
    if (count == 0) {
      fail(done, ud, "edit: old_string not found in %s", path);
    } else {
      fail(done, ud, "edit: old_string occurs %d%s times in %s (must be unique)", count,
           count >= kToolEditCountCap ? "+" : "", path);
    }
    xfree(file_data);
    (void)close(fd);
    return NULL;
  }
  size_t result_len = file_len - old_len + new_len;
  char *result_data = xmalloc(result_len > 0 ? result_len : 1);
  memcpy(result_data, file_data, first);
  memcpy(result_data + first, new_str, new_len);
  memcpy(result_data + first + new_len, file_data + first + old_len, file_len - first - old_len);
  xfree(file_data);
  bool ok = edit_rewrite(fd, path, result_data, result_len, done, ud);
  xfree(result_data);
  if (ok) {
    okf(done, ud, "edit: replaced 1 occurrence in %s", path);
  }
  return NULL;
}

// bash: the one async tool. Three loop handles (process, merged-output pipe,
// one timer serving the timeout and then the post-exit grace); `done` fires
// in the last of the three close callbacks, and the struct dies right after.
struct ToolExec {
  uv_process_t proc;
  uv_pipe_t out;    // read end of the single merged stdout+stderr pipe
  uv_timer_t timer; // timeout role, then re-armed as the post-exit grace
  ToolDoneCb done;
  void *ud;
  Buf output;     // capped at kToolBashOutputCap
  size_t dropped; // bytes drained past the cap (never stop reading)
  size_t nuls;    // NUL bytes stripped (they would truncate the C-string result)
  int64_t timeout_ms;
  int64_t exit_status;
  int term_signal;
  bool exited, pipe_done, timed_out, canceled, done_fired;
  bool timer_closing, grace_armed, grace_forced;
  int closes_done;
  int closes_expected; // 3 on a successful spawn; fewer on setup unwinds
  char scratch[kToolBashScratch];
};

// Output annotations: at most one outcome note + truncation + grace + NULs.
static char *bash_content(ToolExec *exec)
{
  char notes[4][96];
  size_t note_count = 0;
  if (exec->timed_out) {
    (void)snprintf(notes[note_count], sizeof notes[0], "[bash: timed out after %lld ms (SIGKILL)]",
                   (long long)exec->timeout_ms);
    note_count++;
  } else if (exec->canceled) {
    (void)snprintf(notes[note_count], sizeof notes[0], "[bash: canceled]");
    note_count++;
  } else if (exec->term_signal != 0) {
    (void)snprintf(notes[note_count], sizeof notes[0], "[bash: killed by signal %d]",
                   exec->term_signal);
    note_count++;
  } else if (exec->exit_status != 0) {
    (void)snprintf(notes[note_count], sizeof notes[0], "[bash: exit code %lld]",
                   (long long)exec->exit_status);
    note_count++;
  }
  if (exec->dropped > 0) {
    (void)snprintf(notes[note_count], sizeof notes[0], "[bash: output truncated at 64 KiB]");
    note_count++;
  }
  if (exec->grace_forced) {
    (void)snprintf(notes[note_count], sizeof notes[0],
                   "[bash: stopped reading 500 ms after exit; "
                   "a background process still holds the output pipe]");
    note_count++;
  }
  if (exec->nuls > 0) {
    (void)snprintf(notes[note_count], sizeof notes[0], "[bash: %zu NUL bytes removed]", exec->nuls);
    note_count++;
  }
  size_t total = exec->output.size;
  for (size_t i = 0; i < note_count; i++) {
    total += strlen(notes[i]) + 1; // newline joiner (first piece may skip it)
  }
  char *content = xmalloc(total + 1);
  size_t pos = 0;
  if (exec->output.size > 0) {
    memcpy(content, exec->output.data, exec->output.size);
    pos = exec->output.size;
  }
  for (size_t i = 0; i < note_count; i++) {
    if (pos > 0) {
      content[pos] = '\n';
      pos++;
    }
    size_t len = strlen(notes[i]);
    memcpy(content + pos, notes[i], len);
    pos += len;
  }
  content[pos] = '\0';
  return content;
}

static void bash_close_cb(uv_handle_t *handle)
{
  ToolExec *exec = handle->data;
  exec->closes_done++;
  if (exec->closes_done < exec->closes_expected) {
    return;
  }
  if (!exec->done_fired) { // setup-failure paths fired their result inline
    exec->done_fired = true;
    bool is_error =
      exec->timed_out || exec->canceled || exec->term_signal != 0 || exec->exit_status != 0;
    ToolResult result = {.content = bash_content(exec), .is_error = is_error};
    exec->done(exec->ud, &result);
  }
  buf_free(&exec->output);
  xfree(exec);
}

static void bash_grace_cb(uv_timer_t *timer);

// Joins exit and pipe EOF: both done -> close the timer (the third handle);
// exited with the pipe still open -> re-arm the timer as the grace wait.
static void bash_maybe_finish(ToolExec *exec)
{
  if (!exec->exited) {
    return;
  }
  if (exec->pipe_done) {
    if (!exec->timer_closing) {
      exec->timer_closing = true;
      (void)uv_timer_stop(&exec->timer); // cannot fail on an initialized timer
      uv_close((uv_handle_t *)&exec->timer, bash_close_cb);
    }
    return;
  }
  if (!exec->grace_armed) {
    exec->grace_armed = true;
    (void)uv_timer_stop(&exec->timer);
    if (uv_timer_start(&exec->timer, bash_grace_cb, kToolBashGraceMs, 0) != 0) {
      // Cannot wait: take the grace exit now, closing both remaining handles
      // directly rather than re-entering this function.
      exec->grace_forced = true;
      exec->pipe_done = true;
      uv_close((uv_handle_t *)&exec->out, bash_close_cb);
      exec->timer_closing = true;
      uv_close((uv_handle_t *)&exec->timer, bash_close_cb);
    }
  }
}

static void bash_force_pipe_close(ToolExec *exec)
{
  if (exec->pipe_done) {
    return;
  }
  exec->pipe_done = true;
  uv_close((uv_handle_t *)&exec->out, bash_close_cb);
  bash_maybe_finish(exec);
}

static void bash_grace_cb(uv_timer_t *timer)
{
  ToolExec *exec = timer->data;
  exec->grace_forced = true;
  bash_force_pipe_close(exec);
}

static void bash_alloc_cb(uv_handle_t *handle, size_t suggested, uv_buf_t *buf)
{
  (void)suggested;
  ToolExec *exec = handle->data;
  buf->base = exec->scratch; // serial reads: one reusable scratch is legal
  buf->len = sizeof exec->scratch;
}

// Appends to the capped output, stripping NULs and counting overflow. The
// pipe is always drained to EOF: stopping reads would deadlock the child on
// kernel-buffer backpressure.
static void bash_append_output(ToolExec *exec, const char *bytes, size_t len)
{
  size_t pos = 0;
  while (pos < len) { // bounded by len
    const char *nul = memchr(bytes + pos, '\0', len - pos);
    size_t seg = (nul != NULL) ? (size_t)(nul - (bytes + pos)) : len - pos;
    if (seg > 0) {
      size_t room = exec->output.size < exec->output.max ? exec->output.max - exec->output.size : 0;
      size_t take = seg < room ? seg : room;
      if (take > 0) {
        (void)buf_append(&exec->output, bytes + pos, take); // exact fit cannot refuse
      }
      exec->dropped += seg - take;
      pos += seg;
    }
    if (nul != NULL) {
      exec->nuls++;
      pos++; // skip the NUL itself
    }
  }
}

static void bash_read_cb(uv_stream_t *stream, ssize_t nread, const uv_buf_t *buf)
{
  (void)buf; // always exec->scratch
  ToolExec *exec = stream->data;
  if (nread > 0) {
    bash_append_output(exec, exec->scratch, (size_t)nread);
    return;
  }
  if (nread < 0) { // UV_EOF or a read error: either way this pipe is finished
    bash_force_pipe_close(exec);
  }
  // nread == 0: spurious wakeup; keep reading
}

static void bash_exit_cb(uv_process_t *proc, int64_t exit_status, int term_signal)
{
  ToolExec *exec = proc->data;
  exec->exited = true;
  exec->exit_status = exit_status;
  exec->term_signal = term_signal;
  uv_close((uv_handle_t *)&exec->proc, bash_close_cb);
  bash_maybe_finish(exec);
}

static void bash_timeout_cb(uv_timer_t *timer)
{
  ToolExec *exec = timer->data;
  if (exec->exited) {
    return; // raced the exit; the join is already in motion
  }
  exec->timed_out = true;
  int rc = uv_process_kill(&exec->proc, SIGKILL);
  if (rc != 0 && rc != UV_ESRCH) { // ESRCH: it exited as we fired — success
    log_msg(kLogWarn, "bash: kill on timeout: %s", uv_strerror(rc));
  }
}

// Validates command and timeout. The timeout is clamped into
// [1, kToolBashMaxTimeoutMs] regardless of the model's value.
static bool bash_parse_args(cJSON *args, const char **command, int64_t *timeout_ms, ToolDoneCb done,
                            void *ud)
{
  *command = json_get_cstr(args, "command");
  if (*command == NULL || (*command)[0] == '\0') {
    fail(done, ud, "bash: command is required");
    return false;
  }
  *timeout_ms = kToolBashDefaultTimeoutMs;
  const cJSON *item = cJSON_GetObjectItemCaseSensitive(args, "timeout_ms");
  if (item != NULL) {
    if (!cJSON_IsNumber(item)) {
      fail(done, ud, "bash: timeout_ms must be a number");
      return false;
    }
    *timeout_ms = (int64_t)cJSON_GetNumberValue(item);
    if (*timeout_ms < 1) {
      *timeout_ms = 1;
    }
    if (*timeout_ms > kToolBashMaxTimeoutMs) {
      *timeout_ms = kToolBashMaxTimeoutMs;
    }
  }
  return true;
}

// Arms reading and the timeout after a successful spawn. On failure the
// child is killed and the close join finishes silently (done already fired).
static bool bash_arm(ToolExec *exec)
{
  int rc = uv_read_start((uv_stream_t *)&exec->out, bash_alloc_cb, bash_read_cb);
  if (rc != 0) {
    exec->done_fired = true;
    fail(exec->done, exec->ud, "bash: read start: %s", uv_strerror(rc));
    (void)uv_process_kill(&exec->proc, SIGKILL); // ESRCH would mean already dead: fine
    exec->pipe_done = true;
    uv_close((uv_handle_t *)&exec->out, bash_close_cb);
    return false;
  }
  rc = uv_timer_start(&exec->timer, bash_timeout_cb, (uint64_t)exec->timeout_ms, 0);
  if (rc != 0) {
    // The mandatory timeout cannot be armed: do not run unbounded.
    exec->done_fired = true;
    fail(exec->done, exec->ud, "bash: timer start: %s", uv_strerror(rc));
    (void)uv_process_kill(&exec->proc, SIGKILL);
    return false; // reads drain to EOF; exit_cb completes the join
  }
  return true;
}

// The merged-output pipe, CLOEXEC on both ends by hand (macOS has no pipe2);
// the child's copies are dup2'd onto its fds 1/2 by uv_spawn, which clears
// the flag there. The write end stays blocking; uv_pipe_open later makes
// only our read end nonblocking. Fires the failure result itself.
static bool bash_make_pipe(int fds[2], ToolDoneCb done, void *ud)
{
  if (pipe(fds) != 0) {
    fail(done, ud, "bash: pipe: %s", strerror(errno));
    return false;
  }
  if (fcntl(fds[0], F_SETFD, FD_CLOEXEC) != 0 || fcntl(fds[1], F_SETFD, FD_CLOEXEC) != 0) {
    fail(done, ud, "bash: fcntl: %s", strerror(errno));
    (void)close(fds[0]); // unwinding a failed setup
    (void)close(fds[1]);
    return false;
  }
  return true;
}

static ToolExec *bash_execute(cJSON *args, ToolDoneCb done, void *ud)
{
  const char *command = NULL;
  int64_t timeout_ms = 0;
  if (!bash_parse_args(args, &command, &timeout_ms, done, ud)) {
    return NULL;
  }
  int fds[2];
  if (!bash_make_pipe(fds, done, ud)) {
    return NULL;
  }
  ToolExec *exec = xcalloc(1, sizeof *exec);
  exec->done = done;
  exec->ud = ud;
  exec->timeout_ms = timeout_ms;
  buf_init(&exec->output, kToolBashOutputCap);
  exec->proc.data = exec;
  exec->out.data = exec;
  exec->timer.data = exec;
  uv_loop_t *loop = loop_get();
  if (uv_pipe_init(loop, &exec->out, 0) != 0) { // cannot fail on a live loop
    fail(done, ud, "bash: pipe handle init failed");
    (void)close(fds[0]);
    (void)close(fds[1]);
    buf_free(&exec->output);
    xfree(exec);
    return NULL;
  }
  exec->closes_expected = 1; // the pipe handle is registered from here on
  if (uv_pipe_open(&exec->out, fds[0]) != 0) {
    (void)close(fds[0]); // open refused it, so it is still ours
    (void)close(fds[1]);
    exec->done_fired = true;
    fail(done, ud, "bash: pipe open failed");
    uv_close((uv_handle_t *)&exec->out, bash_close_cb);
    return NULL;
  }
  // fds[0] is owned by the pipe handle from here on: never close it by hand.
  if (uv_timer_init(loop, &exec->timer) != 0) { // cannot fail on a live loop
    (void)close(fds[1]);
    exec->done_fired = true;
    fail(done, ud, "bash: timer init failed");
    uv_close((uv_handle_t *)&exec->out, bash_close_cb); // takes fds[0] with it
    return NULL;
  }
  exec->closes_expected = 2;
  uv_stdio_container_t stdio[3];
  stdio[0].flags = UV_IGNORE; // /dev/null stdin on unix
  stdio[1].flags = UV_INHERIT_FD;
  stdio[1].data.fd = fds[1];
  stdio[2].flags = UV_INHERIT_FD;
  stdio[2].data.fd = fds[1]; // same fd on 1 and 2: true interleaving
  // uv_spawn never modifies argv; the cast unconst's the borrowed command,
  // and the fixed entries live in writable storage to satisfy -Wwrite-strings.
  static char sh_path[] = "/bin/sh";
  static char sh_flag[] = "-c";
  char *argv[] = {sh_path, sh_flag, (char *)command, NULL};
  uv_process_options_t options = {
    .file = sh_path,
    .args = argv,
    .exit_cb = bash_exit_cb,
    .stdio_count = 3,
    .stdio = stdio,
  };
  int rc = uv_spawn(loop, &exec->proc, &options);
  (void)close(fds[1]); // ours regardless: the child has its own copies, or never existed
  if (rc != 0) {
    exec->closes_expected = 3; // uv_spawn registers the handle even on failure
    exec->done_fired = true;
    fail(done, ud, "bash: spawn: %s", uv_strerror(rc));
    uv_close((uv_handle_t *)&exec->proc, bash_close_cb);
    uv_close((uv_handle_t *)&exec->out, bash_close_cb);
    exec->timer_closing = true;
    uv_close((uv_handle_t *)&exec->timer, bash_close_cb);
    return NULL;
  }
  exec->closes_expected = 3;
  if (!bash_arm(exec)) {
    return NULL; // done fired inline; the join frees the struct silently
  }
  return exec;
}

static const char READ_SCHEMA[] = //
  "{\"type\":\"object\",\"properties\":{"
  "\"path\":{\"type\":\"string\",\"description\":\"File path to read\"},"
  "\"offset\":{\"type\":\"integer\",\"description\":\"1-based line to start from (default 1)\"},"
  "\"limit\":{\"type\":\"integer\",\"description\":\"Maximum number of lines to return\"}},"
  "\"required\":[\"path\"]}";

static const char WRITE_SCHEMA[] = //
  "{\"type\":\"object\",\"properties\":{"
  "\"path\":{\"type\":\"string\",\"description\":\"File path to write\"},"
  "\"content\":{\"type\":\"string\",\"description\":\"Full file contents to write\"}},"
  "\"required\":[\"path\",\"content\"]}";

static const char EDIT_SCHEMA[] = //
  "{\"type\":\"object\",\"properties\":{"
  "\"path\":{\"type\":\"string\",\"description\":\"File path to edit\"},"
  "\"old_string\":{\"type\":\"string\","
  "\"description\":\"Exact text to replace; must occur exactly once\"},"
  "\"new_string\":{\"type\":\"string\",\"description\":\"Replacement text\"}},"
  "\"required\":[\"path\",\"old_string\",\"new_string\"]}";

static const char BASH_SCHEMA[] = //
  "{\"type\":\"object\",\"properties\":{"
  "\"command\":{\"type\":\"string\",\"description\":\"Shell command run via /bin/sh -c\"},"
  "\"timeout_ms\":{\"type\":\"integer\",\"description\":"
  "\"Kill the command after this many milliseconds (default 30000, max 120000)\"}},"
  "\"required\":[\"command\"]}";

static const ToolDef tool_defs[] = {
  {
    .name = "read",
    .description = "Read a file from disk. Returns the raw contents of the requested "
                   "line window, without line-number prefixes.",
    .params_schema = READ_SCHEMA,
    .mutating = false,
    .execute = read_execute,
  },
  {
    .name = "write",
    .description = "Write content to a file, creating it and any missing parent "
                   "directories; replaces any existing contents entirely.",
    .params_schema = WRITE_SCHEMA,
    .mutating = true,
    .execute = write_execute,
  },
  {
    .name = "edit",
    .description = "Replace one exact occurrence of old_string with new_string in a "
                   "file. Fails unless old_string occurs exactly once; include enough "
                   "surrounding context to make it unique.",
    .params_schema = EDIT_SCHEMA,
    .mutating = true,
    .execute = edit_execute,
  },
  {
    .name = "bash",
    .description = "Run a shell command with stdout and stderr merged into one stream. "
                   "Output is capped at 64 KiB; the command is killed after timeout_ms "
                   "(default 30 s, max 120 s). stdin is /dev/null.",
    .params_schema = BASH_SCHEMA,
    .mutating = true,
    .execute = bash_execute,
  },
};

const ToolDef *tools_lookup(const char *name)
{
  if (name == NULL) {
    return NULL;
  }
  for (size_t i = 0; i < sizeof tool_defs / sizeof tool_defs[0]; i++) {
    if (strcmp(tool_defs[i].name, name) == 0) {
      return &tool_defs[i];
    }
  }
  return NULL;
}

cJSON *tools_build_openai_array(Error *err)
{
  cJSON *tools = cJSON_CreateArray();
  for (size_t i = 0; i < sizeof tool_defs / sizeof tool_defs[0]; i++) {
    const ToolDef *def = &tool_defs[i];
    Error perr = ERROR_INIT;
    cJSON *params = json_parse(cstr_as_string(def->params_schema), kToolSchemaCap, &perr);
    if (params == NULL) {
      api_set_error(err, kErrorTypeException, "tools: %s has an invalid schema: %s", def->name,
                    perr.msg != NULL ? perr.msg : "(no detail)");
      api_clear_error(&perr);
      json_free(tools);
      return NULL;
    }
    cJSON *function = json_new_obj();
    json_add_cstr(function, "name", def->name);
    json_add_cstr(function, "description", def->description);
    cJSON_AddItemToObject(function, "parameters", params);
    cJSON *entry = json_new_obj();
    json_add_cstr(entry, "type", "function");
    cJSON_AddItemToObject(entry, "function", function);
    cJSON_AddItemToArray(tools, entry);
  }
  return tools;
}

void tools_cancel(ToolExec *exec)
{
  if (exec == NULL) {
    return;
  }
  exec->canceled = true;
  if (!exec->exited) {
    int rc = uv_process_kill(&exec->proc, SIGKILL);
    if (rc != 0 && rc != UV_ESRCH) { // ESRCH: exited under us — already done
      log_msg(kLogWarn, "bash: kill on cancel: %s", uv_strerror(rc));
    }
    return; // exit_cb drives the join; done still fires exactly once
  }
  bash_force_pipe_close(exec); // exited, but a holder kept the pipe open
}
