#include "mua/session.h"

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#include "mua/api/private/helpers.h"
#include "mua/json.h"
#include "mua/log.h"
#include "mua/memory.h"
#include "mua/paths.h"

// "YYYYMMDDTHHMMSS_NN" id, "YYYYMMDDTHHMMSS_NN.jsonl" filename.
enum {
  kSessionIdLen = 18,
  kSessionNameLen = 24,
  kSessionScanMax = 65536,  // readdir entries per discovery pass
  kSessionIdAttempts = 100, // the _NN suffix: 00..99
  kSessionEintrMax = 100,   // consecutive EINTR retries, read and write
  kSessionReadChunk = 64 * 1024,
};

struct SessionState {
  char id[kSessionIdLen + 1];
  char *path;      // xmalloc'd
  int fd;          // O_APPEND; owned
  cJSON *messages; // owned array == the conversation
  size_t count;    // mirrors the array length (cJSON_GetArraySize is an O(n) walk)
  bool poisoned;   // a failed write latches every later append to fail fast
  bool needs_heal; // file tail lacks '\n'; the next write prepends one
};

static SessionState *state_alloc(void)
{
  SessionState *sess = xcalloc(1, sizeof *sess);
  sess->fd = -1;
  sess->messages = cJSON_CreateArray();
  return sess;
}

void session_free(SessionState *sess)
{
  if (sess == NULL) {
    return;
  }
  if (sess->fd >= 0 && close(sess->fd) != 0) {
    log_msg(kLogWarn, "session: close %s: %s", sess->path != NULL ? sess->path : "?",
            strerror(errno));
  }
  json_free(sess->messages);
  xfree(sess->path);
  xfree(sess);
}

// state dir + "/sessions", created 0700. xmalloc'd; caller xfrees.
static char *sessions_dir(Error *err)
{
  char *state = paths_state_dir();
  if (state == NULL) {
    api_set_error(err, kErrorTypeException, "session: state dir unresolvable (HOME unset?)");
    return NULL;
  }
  char *dir = paths_join(state, "sessions");
  xfree(state);
  if (!paths_ensure_dir(dir, err)) {
    xfree(dir);
    return NULL;
  }
  return dir;
}

static bool all_digits(const char *s, size_t from, size_t to)
{
  for (size_t i = from; i < to; i++) {
    if (!isdigit((unsigned char)s[i])) {
      return false;
    }
  }
  return true;
}

// YYYYMMDDTHHMMSS_NN.jsonl, fixed width. Shape-filtering discovery rejects
// tmp files and foreign names so strcmp-larger junk can never win.
static bool session_name_ok(const char *name)
{
  return strlen(name) == kSessionNameLen && all_digits(name, 0, 8) && name[8] == 'T' &&
         all_digits(name, 9, 15) && name[15] == '_' && all_digits(name, 16, kSessionIdLen) &&
         strcmp(name + kSessionIdLen, ".jsonl") == 0;
}

// One write(2) per record: [heal '\n'] + line + '\n' lands whole or the
// session poisons -- a partial record on disk must not gain company.
static bool write_line(SessionState *sess, String line, Error *err)
{
  size_t total = line.size + 1 + (sess->needs_heal ? 1 : 0);
  char *buf = xmalloc(total);
  char *dst = buf;
  if (sess->needs_heal) {
    *dst = '\n'; // terminate a torn tail so this record starts a fresh line
    dst++;
  }
  memcpy(dst, line.data, line.size);
  dst[line.size] = '\n';
  ssize_t written = -1;
  for (int tries = 0; tries < kSessionEintrMax; tries++) {
    written = write(sess->fd, buf, total);
    if (written >= 0 || errno != EINTR) {
      break;
    }
  }
  xfree(buf);
  if (written < 0 || (size_t)written != total) {
    sess->poisoned = true;
    api_set_error(err, kErrorTypeException, "session: write %s: %s", sess->path,
                  written < 0 ? strerror(errno) : "short write");
    return false;
  }
  sess->needs_heal = false;
  return true;
}

static bool write_header(SessionState *sess, int64_t created, Error *err)
{
  cJSON *hdr = json_new_obj();
  json_add_cstr(hdr, "type", "session");
  json_add_int(hdr, "version", 1);
  json_add_cstr(hdr, "id", sess->id);
  json_add_int(hdr, "created", created);
  String line = json_print(hdr);
  json_free(hdr);
  bool ok = write_line(sess, line, err);
  xfree(line.data);
  return ok;
}

// Claim an unused "<stamp>_NN.jsonl" with O_CREAT|O_EXCL: the create is the
// existence check, so two processes can never share a file (no TOCTOU).
static int open_fresh(const char *dir, const char *stamp, char *id_out, char **path_out, Error *err)
{
  for (int nn = 0; nn < kSessionIdAttempts; nn++) {
    char name[kSessionNameLen + 1];
    if (snprintf(name, sizeof name, "%s_%02d.jsonl", stamp, nn) != kSessionNameLen) {
      api_set_error(err, kErrorTypeException, "session: bad id stamp %s", stamp);
      return -1;
    }
    char *path = paths_join(dir, name);
    int fd = open(path, O_WRONLY | O_CREAT | O_EXCL | O_APPEND | O_CLOEXEC, 0600);
    if (fd >= 0) {
      memcpy(id_out, name, kSessionIdLen);
      id_out[kSessionIdLen] = '\0';
      *path_out = path;
      return fd;
    }
    int saved = errno;
    xfree(path);
    if (saved != EEXIST) {
      api_set_error(err, kErrorTypeException, "session: create %s/%s: %s", dir, name,
                    strerror(saved));
      return -1;
    }
  }
  api_set_error(err, kErrorTypeException, "session: all %d session ids taken this second",
                kSessionIdAttempts);
  return -1;
}

SessionState *session_new(Error *err)
{
  char *dir = sessions_dir(err);
  if (dir == NULL) {
    return NULL;
  }
  time_t now = time(NULL);
  struct tm tm_utc;
  char stamp[16];
  if (now == (time_t)-1 || gmtime_r(&now, &tm_utc) == NULL ||
      strftime(stamp, sizeof stamp, "%Y%m%dT%H%M%S", &tm_utc) != sizeof stamp - 1) {
    xfree(dir);
    api_set_error(err, kErrorTypeException, "session: cannot format a UTC timestamp");
    return NULL;
  }
  SessionState *sess = state_alloc();
  sess->fd = open_fresh(dir, stamp, sess->id, &sess->path, err);
  xfree(dir);
  if (sess->fd < 0) {
    session_free(sess);
    return NULL;
  }
  if (!write_header(sess, (int64_t)now, err)) {
    // Best effort: a headerless file would fail every later load_latest.
    (void)unlink(sess->path);
    session_free(sess);
    return NULL;
  }
  return sess;
}

typedef struct {
  SessionState *sess;
  size_t line_no;     // of the line being consumed, 1-based
  size_t staged_line; // bad line awaiting the torn-tail verdict; 0 = none
  Error *err;
} LoadScan;

// Validates the line-1 control record and adopts its id.
static bool consume_header(LoadScan *ls, const cJSON *node)
{
  const char *type = json_get_cstr(node, "type");
  const char *id = json_get_cstr(node, "id");
  int64_t version = 0;
  bool shape_ok = type != NULL && strcmp(type, "session") == 0 &&
                  json_get_int(node, "version", &version) && id != NULL && id[0] != '\0' &&
                  strlen(id) <= kSessionIdLen;
  if (!shape_ok) {
    api_set_error(ls->err, kErrorTypeValidation, "session: %s: line 1 is not a session header",
                  ls->sess->path);
    return false;
  }
  if (version != 1) {
    api_set_error(ls->err, kErrorTypeValidation, "session: %s: unsupported version %lld",
                  ls->sess->path, (long long)version);
    return false;
  }
  memcpy(ls->sess->id, id, strlen(id) + 1);
  return true;
}

// One '\n'-terminated line, or the unterminated tail. Returns false only on
// a fatal load error (ls->err set). A line that is neither parseable nor a
// known record shape is staged: fatal if any non-empty line follows, the
// tolerated torn tail if EOF does.
static bool consume_line(LoadScan *ls, String line)
{
  ls->line_no++;
  if (line.size == 0) {
    return true; // blank separators (e.g. around healed tails) are benign
  }
  if (ls->staged_line != 0) {
    api_set_error(ls->err, kErrorTypeValidation, "session: %s: corrupt line %zu", ls->sess->path,
                  ls->staged_line);
    return false; // mid-file corruption desyncs the conversation
  }
  Error perr = ERROR_INIT;
  cJSON *node = json_parse(line, MUA_SESSION_MAX_LINE, &perr);
  api_clear_error(&perr); // a bad line is policy (staging), not an error
  if (ls->line_no == 1) {
    if (node == NULL) {
      api_set_error(ls->err, kErrorTypeValidation, "session: %s: line 1 is not a session header",
                    ls->sess->path);
      return false;
    }
    bool ok = consume_header(ls, node);
    json_free(node);
    return ok;
  }
  if (node == NULL || !cJSON_IsObject(node)) {
    json_free(node); // NULL-safe; a non-object line is corrupt, not a record
    ls->staged_line = ls->line_no;
    return true;
  }
  if (json_get_cstr(node, "role") != NULL) {
    if (ls->sess->count >= MUA_SESSION_MAX_MESSAGES) {
      json_free(node);
      api_set_error(ls->err, kErrorTypeValidation, "session: %s: more than %d messages",
                    ls->sess->path, MUA_SESSION_MAX_MESSAGES);
      return false;
    }
    cJSON_AddItemToArray(ls->sess->messages, node);
    ls->sess->count++;
    return true;
  }
  if (json_get_cstr(node, "type") != NULL) {
    json_free(node); // unknown control records are the tolerated forward slot
    return true;
  }
  json_free(node);
  ls->staged_line = ls->line_no; // an object that is neither message nor control
  return true;
}

// Chunked scan: terminated lines dispatch through consume_line; whatever
// remains in `line` afterwards is the unterminated tail.
static bool load_scan(int fd, size_t size, LoadScan *ls, Buf *line)
{
  char *chunk = xmalloc(kSessionReadChunk);
  size_t remaining = size; // fstat latch: never chase a growing file
  int eintr = 0;
  bool ok = true;
  while (ok && remaining > 0) {
    size_t want = remaining < kSessionReadChunk ? remaining : kSessionReadChunk;
    ssize_t got = read(fd, chunk, want);
    if (got < 0) {
      if (errno == EINTR && ++eintr <= kSessionEintrMax) {
        continue;
      }
      api_set_error(ls->err, kErrorTypeException, "session: read %s: %s", ls->sess->path,
                    strerror(errno));
      ok = false;
      break;
    }
    if (got == 0) {
      break; // shrank since fstat; treat what we have as the whole file
    }
    eintr = 0;
    remaining -= (size_t)got;
    const char *cursor = chunk;
    size_t left = (size_t)got;
    while (left > 0) { // bounded by the chunk size
      const char *nl = memchr(cursor, '\n', left);
      size_t seg = nl != NULL ? (size_t)(nl - cursor) : left;
      if (!buf_append(line, cursor, seg)) {
        api_set_error(ls->err, kErrorTypeValidation, "session: %s: line %zu exceeds %u bytes",
                      ls->sess->path, ls->line_no + 1, (unsigned)MUA_SESSION_MAX_LINE);
        ok = false;
        break;
      }
      if (nl == NULL) {
        break; // the rest of this line arrives with the next chunk
      }
      if (!consume_line(ls, (String){.data = line->data, .size = line->size})) {
        ok = false;
        break;
      }
      buf_reset(line);
      cursor = nl + 1;
      left -= seg + 1;
    }
  }
  xfree(chunk);
  return ok;
}

SessionState *session_load(const char *path, Error *err)
{
  if (path == NULL || path[0] == '\0') {
    api_set_error(err, kErrorTypeValidation, "session: empty session path");
    return NULL;
  }
  int fd = open(path, O_RDWR | O_APPEND | O_CLOEXEC);
  if (fd < 0) {
    api_set_error(err, kErrorTypeException, "session: open %s: %s", path, strerror(errno));
    return NULL;
  }
  struct stat st;
  if (fstat(fd, &st) != 0) {
    api_set_error(err, kErrorTypeException, "session: stat %s: %s", path, strerror(errno));
    (void)close(fd); // nothing useful to add if close also fails
    return NULL;
  }
  if (!S_ISREG(st.st_mode)) {
    api_set_error(err, kErrorTypeValidation, "session: %s is not a regular file", path);
    (void)close(fd);
    return NULL;
  }
  if (st.st_size == 0) {
    api_set_error(err, kErrorTypeValidation, "session: %s: empty file (missing header)", path);
    (void)close(fd);
    return NULL;
  }
  SessionState *sess = state_alloc();
  sess->fd = fd;
  sess->path = xstrdup(path);
  LoadScan ls = {.sess = sess, .err = err};
  Buf line;
  buf_init(&line, MUA_SESSION_MAX_LINE);
  bool ok = load_scan(fd, (size_t)st.st_size, &ls, &line);
  if (ok && line.size > 0) {
    sess->needs_heal = true; // tail lost its '\n'; the next append restores it
    ok = consume_line(&ls, (String){.data = line.data, .size = line.size});
  }
  buf_free(&line);
  if (!ok) {
    session_free(sess);
    return NULL;
  }
  if (ls.staged_line != 0) {
    log_msg(kLogWarn, "session: %s: skipping torn line %zu at end of file", path, ls.staged_line);
  }
  return sess;
}

SessionState *session_load_latest(Error *err)
{
  char *dir = sessions_dir(err);
  if (dir == NULL) {
    return NULL;
  }
  DIR *stream = opendir(dir);
  if (stream == NULL) {
    api_set_error(err, kErrorTypeException, "session: opendir %s: %s", dir, strerror(errno));
    xfree(dir);
    return NULL;
  }
  char best[kSessionNameLen + 1] = "";
  struct dirent *entry = NULL;
  int scanned = 0;
  while (scanned < kSessionScanMax) {
    errno = 0; // readdir returns NULL for both end and error; errno decides
    entry = readdir(stream);
    if (entry == NULL) {
      break;
    }
    scanned++;
    if (session_name_ok(entry->d_name) && strcmp(entry->d_name, best) > 0) {
      memcpy(best, entry->d_name, kSessionNameLen + 1); // fixed width: always fits
    }
  }
  bool read_failed = entry == NULL && errno != 0;
  (void)closedir(stream); // read-only stream; nothing recoverable on failure
  if (read_failed) {
    api_set_error(err, kErrorTypeException, "session: readdir %s: %s", dir, strerror(errno));
    xfree(dir);
    return NULL;
  }
  if (scanned >= kSessionScanMax) {
    log_msg(kLogWarn, "session: %s: discovery stopped after %d entries", dir, kSessionScanMax);
  }
  if (best[0] == '\0') {
    api_set_error(err, kErrorTypeValidation, "session: no sessions to resume");
    xfree(dir);
    return NULL;
  }
  char *path = paths_join(dir, best);
  xfree(dir);
  SessionState *sess = session_load(path, err);
  xfree(path);
  return sess;
}

bool session_append(SessionState *sess, cJSON *msg, Error *err)
{
  // Ownership is unconditional: every path below that does not append frees.
  if (msg == NULL || !cJSON_IsObject(msg) || json_get_cstr(msg, "role") == NULL) {
    json_free(msg);
    api_set_error(err, kErrorTypeValidation,
                  "session: a message is an object with a string \"role\"");
    return false;
  }
  if (sess->poisoned) {
    json_free(msg);
    api_set_error(err, kErrorTypeException, "session: %s: poisoned by an earlier failed write",
                  sess->path);
    return false;
  }
  if (sess->count >= MUA_SESSION_MAX_MESSAGES) {
    json_free(msg);
    api_set_error(err, kErrorTypeValidation, "session: %s: more than %d messages", sess->path,
                  MUA_SESSION_MAX_MESSAGES);
    return false;
  }
  String line = json_print(msg);
  if (line.size > MUA_SESSION_MAX_LINE) {
    xfree(line.data);
    json_free(msg);
    api_set_error(err, kErrorTypeValidation, "session: message of %zu bytes exceeds %u bytes",
                  line.size, (unsigned)MUA_SESSION_MAX_LINE);
    return false;
  }
  bool ok = write_line(sess, line, err);
  xfree(line.data);
  if (!ok) {
    json_free(msg);
    return false;
  }
  cJSON_AddItemToArray(sess->messages, msg); // disk first: the file is the truth
  sess->count++;
  return true;
}

size_t session_message_count(const SessionState *sess)
{
  return sess->count;
}

const cJSON *session_message_get(const SessionState *sess, size_t idx)
{
  if (idx >= sess->count) {
    return NULL;
  }
  return cJSON_GetArrayItem(sess->messages, (int)idx);
}

const cJSON *session_messages(const SessionState *sess)
{
  return sess->messages;
}

const char *session_id(const SessionState *sess)
{
  return sess->id;
}
