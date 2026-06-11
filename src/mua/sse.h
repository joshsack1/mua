#ifndef MUA_SSE_H
#define MUA_SSE_H

#include <stdbool.h>
#include <stddef.h>

#include "mua/api/private/defs.h"

// Incremental SSE (Server-Sent Events) decoder: consumes the response body in
// arbitrary-size chunks (libcurl write callbacks align with nothing) and emits
// complete events. Byte-oriented end to end — multi-byte UTF-8 sequences that
// split across chunks reassemble for free because bytes are buffered, never
// decoded. Do NOT add incremental text decoding anywhere in this path.
//
// WHATWG EventSource processing model subset: `event:`/`data:`/`id:` fields,
// `retry:` ignored (reconnection is provider retry policy, not EventSource
// emulation), `:` comment lines ignored, multi-line data joined with '\n',
// dispatch on blank line, line terminators '\n', '\r\n', and bare '\r'
// (including a '\r''\n' pair split across chunk boundaries), one optional
// leading UTF-8 BOM.

typedef struct SseParser SseParser;

// Hard caps; 0 selects the default. Constructor parameters so tests can
// shrink them and exercise overflow without megabyte fixtures.
typedef struct {
  size_t max_line;       // one logical line; default MUA_SSE_MAX_LINE
  size_t max_event_data; // accumulated data per event; default MUA_SSE_MAX_EVENT_DATA
  size_t max_event_type; // default MUA_SSE_MAX_EVENT_TYPE
  size_t max_id;         // default MUA_SSE_MAX_ID
} SseLimits;

#define MUA_SSE_MAX_LINE (256 * 1024)
#define MUA_SSE_MAX_EVENT_DATA (1024 * 1024)
#define MUA_SSE_MAX_EVENT_TYPE 1024
#define MUA_SSE_MAX_ID 1024

// Strings are BORROWED views, valid only for the duration of the callback.
// `event_type` is "message" when the event named none. `id` is the last seen
// event id (empty if none yet; ids persist across events per spec). Return
// false to stop parsing: the parser latches an aborted state and refuses
// further input. (Pointers rather than by-value Strings so the test harness
// can implement the callback through LuaJIT FFI, which cannot build
// callbacks with aggregate parameters.)
typedef bool (*SseEventCb)(void *ud, const String *event_type, const String *data,
                           const String *id);

SseParser *sse_parser_new(const SseLimits *limits, SseEventCb cb, void *ud);
void sse_parser_free(SseParser *parser);

// Consumes exactly `len` bytes, emitting any events they complete. On a cap
// overflow the parser latches a terminal failed state (kErrorTypeValidation)
// and every later feed fails fast with the same error; after a callback abort
// it latches kErrorTypeException the same way. A failed stream is dead — the
// caller aborts the transfer; there is no resynchronization.
bool sse_parser_feed(SseParser *parser, const char *bytes, size_t len, Error *err);

typedef enum {
  kSseEofClean = 0, // no partial line, no un-dispatched event pending
  kSseEofTruncated, // stream ended mid-line or mid-event (or failed/aborted)
} SseEof;

SseEof sse_parser_finish(const SseParser *parser);

// Back to the start-of-stream state for retry reuse; buffer capacity is kept,
// any latched failure is cleared.
void sse_parser_reset(SseParser *parser);

#endif // MUA_SSE_H
