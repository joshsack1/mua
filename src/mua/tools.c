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

enum {
  kToolReadReturnCap = 256 * 1024,    // window bytes handed back to the model
  kToolReadScanCap = 8 * 1024 * 1024, // bytes examined on disk per call
  kToolReadChunk = 64 * 1024,
  kToolSchemaCap = 16 * 1024, // parse bound for the static builtin schemas
  kToolEintrMax = 100,        // consecutive EINTR retries
};

// Fire a printf-formatted failure result; the model sees this text verbatim.
static void fail(ToolDoneCb done, void *ud, const char *fmt, ...) MUA_PRINTF_ATTR(3, 4);

static void fail(ToolDoneCb done, void *ud, const char *fmt, ...)
{
  va_list ap;
  va_start(ap, fmt);
  va_list ap2;
  va_copy(ap2, ap);
  int need = vsnprintf(NULL, 0, fmt, ap);
  va_end(ap);
  if (need < 0) {
    need = 0; // a formatting failure degrades to an empty message, not UB
  }
  char *msg = xmalloc((size_t)need + 1);
  msg[0] = '\0';
  (void)vsnprintf(msg, (size_t)need + 1, fmt, ap2); // sized by the first pass
  va_end(ap2);
  ToolResult result = {.content = msg, .is_error = true};
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

static const char READ_SCHEMA[] = //
  "{\"type\":\"object\",\"properties\":{"
  "\"path\":{\"type\":\"string\",\"description\":\"File path to read\"},"
  "\"offset\":{\"type\":\"integer\",\"description\":\"1-based line to start from (default 1)\"},"
  "\"limit\":{\"type\":\"integer\",\"description\":\"Maximum number of lines to return\"}},"
  "\"required\":[\"path\"]}";

static const ToolDef tool_defs[] = {
  {
    .name = "read",
    .description = "Read a file from disk. Returns the raw contents of the requested "
                   "line window, without line-number prefixes.",
    .params_schema = READ_SCHEMA,
    .mutating = false,
    .execute = read_execute,
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
