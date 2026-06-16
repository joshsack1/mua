#ifndef MUA_MSGPACK_H
#define MUA_MSGPACK_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "mua/api/private/defs.h"

// Object <-> msgpack: the third marshaling axis (after Object<->Lua in bridge.c
// and cJSON<->Object in json.c), used only by the --embed RPC server. Like the
// others it is an iterative, depth-capped tree walk (kMarshalDepthCap) -- never
// recursion on untrusted input -- and treats String as length-prefixed bytes,
// never NUL-terminated. Only the msgpack types that map to Object are handled;
// ext (and the never-used 0xc1) are rejected. bin decodes to String; map keys
// must be strings (Object Dict keys are Strings). uint64 above INT64_MAX decodes
// to Float rather than overflow Integer.

// A growable encode buffer. Zero-initialize with MSGPACK_BUFFER_INIT; release
// with msgpack_buffer_free. `data` is xmalloc'd (xfree it, or use the helper).
typedef struct {
  uint8_t *data;
  size_t size;
  size_t cap;
} MsgpackBuffer;

#define MSGPACK_BUFFER_INIT ((MsgpackBuffer){.data = NULL, .size = 0, .cap = 0})

void msgpack_buffer_free(MsgpackBuffer *buf);

// Appends the msgpack encoding of one `obj` to `buf` (grown via xrealloc, which
// aborts on OOM, so there is no failure path). Depth is bounded by the Object's
// producers, so deeper than the cap cannot occur (asserted).
void msgpack_encode(MsgpackBuffer *buf, const Object *obj);

// Appends an array header announcing `n` elements; the caller then encodes the
// elements (used to frame an RPC response without building a wrapper Object).
void msgpack_encode_array_header(MsgpackBuffer *buf, size_t n);

typedef enum {
  kMsgpackOk = 0,     // one complete value decoded; *consumed bytes were used
  kMsgpackIncomplete, // buf holds only a truncated prefix -- read more and retry
  kMsgpackError,      // malformed, an unmodeled type, or nesting past the cap
} MsgpackStatus;

// Decodes one msgpack value from buf[0, len). On kMsgpackOk sets *out (owned;
// free via api_free_object) and *consumed (the value's byte length). On
// kMsgpackIncomplete the buffer is a prefix (decode again with more bytes); a
// declared container size larger than the remaining bytes is treated as
// incomplete, never allocated. On kMsgpackError sets `err`. *out/*consumed are
// meaningful only for kMsgpackOk.
MsgpackStatus msgpack_to_object(const uint8_t *buf, size_t len, size_t *consumed, Object *out,
                                Error *err);

#endif // MUA_MSGPACK_H
