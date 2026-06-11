#include "mua/sse.h"

#include <string.h>

#include "mua/api/private/helpers.h"
#include "mua/memory.h"

typedef enum {
  kSseStateBom = 0, // consuming the optional 3-byte BOM (may span feeds)
  kSseStateLine,    // scanning for line terminators
  kSseStateFailed,  // terminal: cap overflow (latched)
  kSseStateAborted, // terminal: event callback returned false
} SseState;

struct SseParser {
  SseLimits limits;
  SseEventCb cb;
  void *ud;
  SseState state;
  unsigned bom_matched; // bytes of the BOM seen so far (0..3)
  bool pending_lf_skip; // last consumed byte was a line-ending '\r' at end of input
  Buf carry;            // unterminated tail of the current line, nothing else
  Buf data;
  Buf event_type;
  Buf id;
  const char *fail_msg; // static string; valid while state is Failed/Aborted
  ErrorType fail_type;
};

static const char bom_bytes[3] = {(char)0xef, (char)0xbb, (char)0xbf};

static bool fail(SseParser *parser, ErrorType type, const char *msg, Error *err)
{
  parser->state = kSseStateFailed;
  parser->fail_msg = msg;
  parser->fail_type = type;
  api_set_error(err, type, "sse: %s", msg);
  return false;
}

// Dispatches the accumulated event on a blank line (WHATWG step: an empty
// data buffer dispatches nothing and clears the type; ids persist).
static bool dispatch_event(SseParser *parser)
{
  if (parser->data.size == 0) {
    buf_reset(&parser->event_type);
    return true;
  }
  parser->data.size--; // remove the single trailing '\n' the last data line appended
  static char default_type[] = "message";
  String event_type = (String){.data = default_type, .size = sizeof(default_type) - 1};
  if (parser->event_type.size > 0) {
    event_type = (String){.data = parser->event_type.data, .size = parser->event_type.size};
  }
  String data = (String){.data = parser->data.data, .size = parser->data.size};
  String id = (String){.data = parser->id.data, .size = parser->id.size};
  bool keep_going = parser->cb(parser->ud, &event_type, &data, &id);
  buf_reset(&parser->data);
  buf_reset(&parser->event_type);
  if (!keep_going) {
    parser->state = kSseStateAborted;
  }
  return keep_going;
}

static bool process_field(SseParser *parser, const char *name, size_t name_len, const char *value,
                          size_t value_len, Error *err)
{
  if (name_len == 4 && memcmp(name, "data", 4) == 0) {
    if (!buf_append(&parser->data, value, value_len) || !buf_append(&parser->data, "\n", 1)) {
      return fail(parser, kErrorTypeValidation, "event data exceeds limit", err);
    }
  } else if (name_len == 5 && memcmp(name, "event", 5) == 0) {
    buf_reset(&parser->event_type);
    if (!buf_append(&parser->event_type, value, value_len)) {
      return fail(parser, kErrorTypeValidation, "event type exceeds limit", err);
    }
  } else if (name_len == 2 && memcmp(name, "id", 2) == 0) {
    if (memchr(value, '\0', value_len) == NULL) { // ids containing NUL are ignored per spec
      buf_reset(&parser->id);
      if (!buf_append(&parser->id, value, value_len)) {
        return fail(parser, kErrorTypeValidation, "event id exceeds limit", err);
      }
    }
  }
  // "retry" and unknown fields are ignored.
  return true;
}

static bool process_line(SseParser *parser, const char *line, size_t len, Error *err)
{
  if (len == 0) {
    if (!dispatch_event(parser)) {
      // dispatch_event already latched kSseStateAborted; record the reason.
      parser->fail_msg = "aborted by event callback";
      parser->fail_type = kErrorTypeException;
      api_set_error(err, kErrorTypeException, "sse: %s", parser->fail_msg);
      return false;
    }
    return true;
  }
  if (line[0] == ':') {
    return true; // comment (e.g. ": OPENROUTER PROCESSING" keep-alives)
  }
  const char *colon = memchr(line, ':', len);
  size_t name_len = (colon != NULL) ? (size_t)(colon - line) : len;
  const char *value = "";
  size_t value_len = 0;
  if (colon != NULL) {
    value = colon + 1;
    value_len = len - name_len - 1;
    if (value_len > 0 && value[0] == ' ') { // exactly one leading space is stripped
      value++;
      value_len--;
    }
  }
  return process_field(parser, line, name_len, value, value_len, err);
}

// Consumes BOM-prefix bytes; on mismatch the matched prefix becomes content.
static bool bom_step(SseParser *parser, const char *bytes, size_t len, size_t *cursor, Error *err)
{
  while (*cursor < len && parser->state == kSseStateBom) {
    if (bytes[*cursor] == bom_bytes[parser->bom_matched]) {
      parser->bom_matched++;
      (*cursor)++;
      if (parser->bom_matched == 3) {
        parser->state = kSseStateLine;
      }
    } else {
      // Not a BOM after all: the matched prefix is line content. BOM bytes
      // are never CR/LF, so they simply join the carry buffer.
      if (!buf_append(&parser->carry, bom_bytes, parser->bom_matched)) {
        return fail(parser, kErrorTypeValidation, "line exceeds limit", err);
      }
      parser->bom_matched = 0;
      parser->state = kSseStateLine;
    }
  }
  return true;
}

static const char *find_terminator(const char *bytes, size_t len)
{
  const char *cr = memchr(bytes, '\r', len);
  const char *lf = memchr(bytes, '\n', len);
  if (cr == NULL) {
    return lf;
  }
  if (lf == NULL) {
    return cr;
  }
  return (cr < lf) ? cr : lf;
}

// Consumes one line (or the unterminated tail) from bytes[*cursor..len).
static bool scan_line(SseParser *parser, const char *bytes, size_t len, size_t *cursor, Error *err)
{
  const char *span = bytes + *cursor;
  size_t span_len = len - *cursor;
  const char *term = find_terminator(span, span_len);
  if (term == NULL) {
    if (!buf_append(&parser->carry, span, span_len)) {
      return fail(parser, kErrorTypeValidation, "line exceeds limit", err);
    }
    *cursor = len;
    return true;
  }
  size_t line_len = (size_t)(term - span);
  const char *line = span;
  if (parser->carry.size > 0) {
    if (!buf_append(&parser->carry, span, line_len)) {
      return fail(parser, kErrorTypeValidation, "line exceeds limit", err);
    }
    line = parser->carry.data;
    line_len = parser->carry.size;
  } else if (line_len > parser->limits.max_line) {
    // The zero-copy path must enforce the same cap the carry path would,
    // or the failure would depend on how the stream happened to chunk.
    return fail(parser, kErrorTypeValidation, "line exceeds limit", err);
  }
  if (!process_line(parser, line, line_len, err)) {
    return false;
  }
  buf_reset(&parser->carry);
  *cursor = (size_t)(term - bytes) + 1;
  if (*term == '\r') {
    if (*cursor < len) {
      if (bytes[*cursor] == '\n') {
        (*cursor)++;
      }
    } else {
      // '\r' was the last available byte: a '\n' opening the next feed
      // belongs to this terminator, not to a new (phantom) blank line.
      parser->pending_lf_skip = true;
    }
  }
  return true;
}

SseParser *sse_parser_new(const SseLimits *limits, SseEventCb cb, void *ud)
{
  SseParser *parser = xcalloc(1, sizeof(*parser));
  SseLimits resolved = (limits != NULL) ? *limits : (SseLimits){0};
  if (resolved.max_line == 0) {
    resolved.max_line = MUA_SSE_MAX_LINE;
  }
  if (resolved.max_event_data == 0) {
    resolved.max_event_data = MUA_SSE_MAX_EVENT_DATA;
  }
  if (resolved.max_event_type == 0) {
    resolved.max_event_type = MUA_SSE_MAX_EVENT_TYPE;
  }
  if (resolved.max_id == 0) {
    resolved.max_id = MUA_SSE_MAX_ID;
  }
  parser->limits = resolved;
  parser->cb = cb;
  parser->ud = ud;
  parser->state = kSseStateBom;
  buf_init(&parser->carry, resolved.max_line);
  buf_init(&parser->data, resolved.max_event_data);
  buf_init(&parser->event_type, resolved.max_event_type);
  buf_init(&parser->id, resolved.max_id);
  return parser;
}

void sse_parser_free(SseParser *parser)
{
  if (parser == NULL) {
    return;
  }
  buf_free(&parser->carry);
  buf_free(&parser->data);
  buf_free(&parser->event_type);
  buf_free(&parser->id);
  xfree(parser);
}

bool sse_parser_feed(SseParser *parser, const char *bytes, size_t len, Error *err)
{
  if (parser->state == kSseStateFailed || parser->state == kSseStateAborted) {
    api_set_error(err, parser->fail_type, "sse: %s", parser->fail_msg);
    return false;
  }
  size_t cursor = 0;
  if (parser->pending_lf_skip) {
    if (len == 0) {
      return true; // nothing to inspect; the skip stays pending
    }
    parser->pending_lf_skip = false;
    if (bytes[0] == '\n') {
      cursor = 1;
    }
  }
  // Bounded by construction: every iteration strictly advances `cursor`.
  while (cursor < len) {
    if (parser->state == kSseStateBom) {
      if (!bom_step(parser, bytes, len, &cursor, err)) {
        return false;
      }
    } else if (!scan_line(parser, bytes, len, &cursor, err)) {
      return false;
    }
  }
  return true;
}

SseEof sse_parser_finish(const SseParser *parser)
{
  if (parser->state == kSseStateFailed || parser->state == kSseStateAborted) {
    return kSseEofTruncated;
  }
  if (parser->carry.size > 0 || parser->data.size > 0 || parser->bom_matched > 0) {
    return kSseEofTruncated;
  }
  return kSseEofClean;
}

void sse_parser_reset(SseParser *parser)
{
  buf_reset(&parser->carry);
  buf_reset(&parser->data);
  buf_reset(&parser->event_type);
  buf_reset(&parser->id);
  parser->state = kSseStateBom;
  parser->bom_matched = 0;
  parser->pending_lf_skip = false;
  parser->fail_msg = NULL;
  parser->fail_type = kErrorTypeNone;
}
