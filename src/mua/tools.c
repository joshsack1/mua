#include "mua/tools.h"

#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include "mua/api/private/helpers.h"
#include "mua/json.h"
#include "mua/memory.h"
#include "mua/paths.h"

enum {
  kToolReadReturnCap = 256 * 1024,    // window bytes handed back to the model
  kToolReadScanCap = 8 * 1024 * 1024, // bytes examined on disk per call
  kToolReadChunk = 64 * 1024,
  kToolEditFileCap = 4 * 1024 * 1024, // whole-file rewrite bound
  kToolEditCountCap = 100,            // occurrence counting stops here
  kToolSchemaCap = 16 * 1024,         // parse bound for the static builtin schemas
  kToolEintrMax = 100,                // consecutive EINTR retries
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
  (void)exec; // no async tool exists yet; bash supplies the first ToolExec
}
