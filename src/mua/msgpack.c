#include "mua/msgpack.h"

#include <assert.h>
#include <string.h>

#include "mua/api/private/helpers.h"
#include "mua/memory.h"

// --- Encode -------------------------------------------------------------------
// Append helpers grow the buffer through xrealloc (aborts on OOM), so encoding
// has no failure path. Integers take the shortest signed/unsigned form, doubles
// always go as float64, strings/arrays/maps take the smallest header.

void msgpack_buffer_free(MsgpackBuffer *buf)
{
  xfree(buf->data);
  *buf = MSGPACK_BUFFER_INIT;
}

static void mp_reserve(MsgpackBuffer *buf, size_t extra)
{
  if (buf->size + extra <= buf->cap) {
    return;
  }
  size_t cap = buf->cap != 0 ? buf->cap : 64;
  while (cap < buf->size + extra) {
    cap *= 2;
  }
  buf->data = xrealloc(buf->data, cap);
  buf->cap = cap;
}

static void mp_put(MsgpackBuffer *buf, uint8_t byte)
{
  mp_reserve(buf, 1);
  buf->data[buf->size++] = byte;
}

static void mp_put_bytes(MsgpackBuffer *buf, const void *src, size_t n)
{
  mp_reserve(buf, n);
  if (n > 0) {
    memcpy(buf->data + buf->size, src, n);
    buf->size += n;
  }
}

// Appends the low `nbytes` of `v` big-endian (msgpack is big-endian).
static void mp_put_be(MsgpackBuffer *buf, uint64_t v, int nbytes)
{
  for (int i = nbytes - 1; i >= 0; i--) {
    mp_put(buf, (uint8_t)(v >> (8 * i)));
  }
}

static void mp_emit_int(MsgpackBuffer *buf, int64_t v)
{
  if (v >= -32 && v <= 0x7f) {
    mp_put(buf, (uint8_t)v); // fixint (positive or negative): one byte
  } else if (v >= 0) {
    if (v <= 0xff) {
      mp_put(buf, 0xcc);
      mp_put(buf, (uint8_t)v); // uint8
    } else if (v <= 0xffff) {
      mp_put(buf, 0xcd);
      mp_put_be(buf, (uint64_t)v, 2);
    } else if (v <= 0xffffffffLL) {
      mp_put(buf, 0xce);
      mp_put_be(buf, (uint64_t)v, 4);
    } else {
      mp_put(buf, 0xcf);
      mp_put_be(buf, (uint64_t)v, 8);
    }
  } else if (v >= -128) {
    mp_put(buf, 0xd0);
    mp_put(buf, (uint8_t)v); // int8
  } else if (v >= -32768) {
    mp_put(buf, 0xd1);
    mp_put_be(buf, (uint64_t)v, 2);
  } else if (v >= -2147483648LL) {
    mp_put(buf, 0xd2);
    mp_put_be(buf, (uint64_t)v, 4);
  } else {
    mp_put(buf, 0xd3);
    mp_put_be(buf, (uint64_t)v, 8);
  }
}

// Appends a length-prefixed count with the given format bytes (str/array/map all
// share this fix/16/32 shape; str passes the fixstr base, arrays/maps theirs).
static void mp_emit_count(MsgpackBuffer *buf, size_t n, uint8_t fix, uint8_t f16, uint8_t f32)
{
  assert(n <= 0xffffffffULL);              // 4 Gi entries cannot occur for our data
  size_t fixmax = fix == 0xa0 ? 0x1f : 0x0f; // fixstr is a 5-bit length; fixarray/fixmap 4-bit
  if (n <= fixmax) {
    mp_put(buf, (uint8_t)(fix | n));
  } else if (n <= 0xffff) {
    mp_put(buf, f16);
    mp_put_be(buf, n, 2);
  } else {
    mp_put(buf, f32);
    mp_put_be(buf, n, 4);
  }
}

static void mp_emit_str(MsgpackBuffer *buf, String s)
{
  mp_emit_count(buf, s.size, 0xa0, 0xda, 0xdb);
  mp_put_bytes(buf, s.data, s.size);
}

void msgpack_encode_array_header(MsgpackBuffer *buf, size_t n)
{
  mp_emit_count(buf, n, 0x90, 0xdc, 0xdd);
}

// Appends a scalar fully, or a container's header (the walk fills its elements).
static void mp_emit_node(MsgpackBuffer *buf, const Object *obj)
{
  switch (obj->type) {
    case kObjectTypeNil:
    case kObjectTypeSession: // no wire form; encode as nil
      mp_put(buf, 0xc0);
      break;
    case kObjectTypeBoolean:
      mp_put(buf, obj->data.boolean ? 0xc3 : 0xc2);
      break;
    case kObjectTypeInteger:
      mp_emit_int(buf, obj->data.integer);
      break;
    case kObjectTypeFloat: {
      uint64_t bits;
      memcpy(&bits, &obj->data.floating, 8);
      mp_put(buf, 0xcb);
      mp_put_be(buf, bits, 8);
      break;
    }
    case kObjectTypeString:
      mp_emit_str(buf, obj->data.string);
      break;
    case kObjectTypeArray:
      msgpack_encode_array_header(buf, obj->data.array.size);
      break;
    case kObjectTypeDict:
      mp_emit_count(buf, obj->data.dict.size, 0x80, 0xde, 0xdf);
      break;
  }
}

typedef struct {
  const Object *container; // source Array/Dict
  bool is_map;
  size_t n;
  size_t next;
} MpEncFrame;

static MpEncFrame mp_enc_frame(const Object *obj)
{
  bool is_map = obj->type == kObjectTypeDict;
  return (MpEncFrame){.container = obj,
                      .is_map = is_map,
                      .n = is_map ? obj->data.dict.size : obj->data.array.size,
                      .next = 0};
}

void msgpack_encode(MsgpackBuffer *buf, const Object *obj)
{
  mp_emit_node(buf, obj);
  if (obj->type != kObjectTypeArray && obj->type != kObjectTypeDict) {
    return; // scalar: fully emitted
  }
  MpEncFrame stack[kMarshalDepthCap];
  int depth = 0;
  stack[depth++] = mp_enc_frame(obj);
  while (depth > 0) {
    MpEncFrame *f = &stack[depth - 1];
    if (f->next >= f->n) {
      depth--;
      continue;
    }
    size_t i = f->next++;
    const Object *child;
    if (f->is_map) {
      mp_emit_str(buf, f->container->data.dict.items[i].key); // key precedes its value
      child = &f->container->data.dict.items[i].value;
    } else {
      child = &f->container->data.array.items[i];
    }
    mp_emit_node(buf, child);
    if (child->type == kObjectTypeArray || child->type == kObjectTypeDict) {
      assert(depth < kMarshalDepthCap); // producers cap depth; deeper cannot occur
      stack[depth++] = mp_enc_frame(child);
    }
  }
}

// --- Decode -------------------------------------------------------------------
// Iterative, depth-capped, and partial-input-aware: a value spanning more bytes
// than `buf` holds is kMsgpackIncomplete, and a container declaring more elements
// than the remaining bytes (each element is >= 1 byte) is likewise incomplete --
// never allocated, so a lying huge count cannot exhaust memory.

static uint64_t mp_be(const uint8_t *p, int nbytes)
{
  uint64_t v = 0;
  for (int i = 0; i < nbytes; i++) {
    v = (v << 8) | p[i];
  }
  return v;
}

// Reads the `nbytes`-wide big-endian payload after the format byte at *pos into
// *out, advancing *pos past both. Incomplete if the bytes are not all present.
static MsgpackStatus mp_take_uint(const uint8_t *buf, size_t len, size_t *pos, int nbytes,
                                  uint64_t *out)
{
  if (len - *pos < (size_t)nbytes + 1) {
    return kMsgpackIncomplete;
  }
  *out = mp_be(buf + *pos + 1, nbytes);
  *pos += (size_t)nbytes + 1;
  return kMsgpackOk;
}

// Reads a `payload`-byte string/bin body that starts `hdr` bytes after *pos into
// an owned String. Incomplete if header or body bytes are missing.
static MsgpackStatus mp_take_str(const uint8_t *buf, size_t len, size_t *pos, size_t hdr,
                                 size_t payload, Object *slot)
{
  if (len - *pos < hdr || len - *pos - hdr < payload) {
    return kMsgpackIncomplete;
  }
  char *data = NULL;
  if (payload > 0) {
    data = xmalloc(payload);
    memcpy(data, buf + *pos + hdr, payload);
  }
  *slot = STRING_OBJ(((String){.data = data, .size = payload}));
  *pos += hdr + payload;
  return kMsgpackOk;
}

typedef struct {
  Object *container;   // array or dict being filled (Nil-initialized backing)
  size_t remaining;    // elements (array) or pairs (map) still to read
  size_t idx;          // next element/pair index
  bool is_map;
  bool awaiting_value; // map only: a key has been read, its value is next
} MpDecFrame;

// Initializes *slot as an Array/Dict of `count` Nil-filled slots and pushes its
// frame. Incomplete (never allocated) if count exceeds the bytes that remain.
static MsgpackStatus mp_open(size_t len, size_t *pos, size_t hdr, uint64_t count, bool is_map,
                             Object *slot, MpDecFrame *stack, int *depth, Error *err)
{
  if (count > len - *pos - hdr) {
    return kMsgpackIncomplete; // each element is >= 1 byte: cannot be complete yet
  }
  if (*depth >= kMarshalDepthCap) {
    api_set_error(err, kErrorTypeValidation, "msgpack nesting exceeds %d", kMarshalDepthCap);
    return kMsgpackError;
  }
  if (is_map) {
    *slot = DICT_OBJ(((Dict){.items = count > 0 ? xcalloc(count, sizeof(KeyValuePair)) : NULL,
                             .size = count,
                             .capacity = count}));
  } else {
    *slot = ARRAY_OBJ(((Array){.items = count > 0 ? xcalloc(count, sizeof(Object)) : NULL,
                               .size = count,
                               .capacity = count}));
  }
  *pos += hdr;
  stack[(*depth)++] =
    (MpDecFrame){.container = slot, .is_map = is_map, .remaining = count, .idx = 0};
  return kMsgpackOk;
}

// Decodes the fixed-width number formats 0xca..0xd3 (float32/64, uint and int of
// every width). uint64 above INT64_MAX becomes Float rather than overflowing.
static MsgpackStatus mp_read_number(uint8_t b, const uint8_t *buf, size_t len, size_t *pos,
                                    Object *slot)
{
  uint64_t u = 0;
  MsgpackStatus s = kMsgpackOk;
  switch (b) {
    case 0xca: // float32
      s = mp_take_uint(buf, len, pos, 4, &u);
      if (s == kMsgpackOk) {
        uint32_t bits = (uint32_t)u;
        float f;
        memcpy(&f, &bits, 4);
        *slot = FLOAT_OBJ((double)f);
      }
      return s;
    case 0xcb: // float64
      s = mp_take_uint(buf, len, pos, 8, &u);
      if (s == kMsgpackOk) {
        double d;
        memcpy(&d, &u, 8);
        *slot = FLOAT_OBJ(d);
      }
      return s;
    case 0xcc: // uint8
      s = mp_take_uint(buf, len, pos, 1, &u);
      *slot = INTEGER_OBJ((int64_t)(uint8_t)u);
      return s;
    case 0xcd: // uint16
      s = mp_take_uint(buf, len, pos, 2, &u);
      *slot = INTEGER_OBJ((int64_t)(uint16_t)u);
      return s;
    case 0xce: // uint32
      s = mp_take_uint(buf, len, pos, 4, &u);
      *slot = INTEGER_OBJ((int64_t)(uint32_t)u);
      return s;
    case 0xcf: // uint64
      s = mp_take_uint(buf, len, pos, 8, &u);
      *slot = u <= (uint64_t)INT64_MAX ? INTEGER_OBJ((int64_t)u) : FLOAT_OBJ((double)u);
      return s;
    case 0xd0: // int8
      s = mp_take_uint(buf, len, pos, 1, &u);
      *slot = INTEGER_OBJ((int8_t)u);
      return s;
    case 0xd1: // int16
      s = mp_take_uint(buf, len, pos, 2, &u);
      *slot = INTEGER_OBJ((int16_t)u);
      return s;
    case 0xd2: // int32
      s = mp_take_uint(buf, len, pos, 4, &u);
      *slot = INTEGER_OBJ((int32_t)u);
      return s;
    default: // 0xd3: int64
      s = mp_take_uint(buf, len, pos, 8, &u);
      *slot = INTEGER_OBJ((int64_t)u);
      return s;
  }
}

// Decodes the formats 0xc4..0xdf (bin, the 0xca..0xd3 numbers, str, array, map).
// 0xc1 (never used) and the ext family fall through to a Validation error.
static MsgpackStatus mp_read_ext(uint8_t b, const uint8_t *buf, size_t len, size_t *pos,
                                 Object *slot, MpDecFrame *stack, int *depth, Error *err)
{
  if (b >= 0xca && b <= 0xd3) {
    return mp_read_number(b, buf, len, pos, slot);
  }
  switch (b) {
    case 0xc4: // bin8
      return len - *pos < 2 ? kMsgpackIncomplete : mp_take_str(buf, len, pos, 2, buf[*pos + 1], slot);
    case 0xc5: // bin16
      return len - *pos < 3 ? kMsgpackIncomplete
                            : mp_take_str(buf, len, pos, 3, mp_be(buf + *pos + 1, 2), slot);
    case 0xc6: // bin32
      return len - *pos < 5 ? kMsgpackIncomplete
                            : mp_take_str(buf, len, pos, 5, mp_be(buf + *pos + 1, 4), slot);
    case 0xd9: // str8
      return len - *pos < 2 ? kMsgpackIncomplete : mp_take_str(buf, len, pos, 2, buf[*pos + 1], slot);
    case 0xda: // str16
      return len - *pos < 3 ? kMsgpackIncomplete
                            : mp_take_str(buf, len, pos, 3, mp_be(buf + *pos + 1, 2), slot);
    case 0xdb: // str32
      return len - *pos < 5 ? kMsgpackIncomplete
                            : mp_take_str(buf, len, pos, 5, mp_be(buf + *pos + 1, 4), slot);
    case 0xdc: // array16
      return len - *pos < 3 ? kMsgpackIncomplete
                            : mp_open(len, pos, 3, mp_be(buf + *pos + 1, 2), false, slot, stack,
                                      depth, err);
    case 0xdd: // array32
      return len - *pos < 5 ? kMsgpackIncomplete
                            : mp_open(len, pos, 5, mp_be(buf + *pos + 1, 4), false, slot, stack,
                                      depth, err);
    case 0xde: // map16
      return len - *pos < 3 ? kMsgpackIncomplete
                            : mp_open(len, pos, 3, mp_be(buf + *pos + 1, 2), true, slot, stack,
                                      depth, err);
    case 0xdf: // map32
      return len - *pos < 5 ? kMsgpackIncomplete
                            : mp_open(len, pos, 5, mp_be(buf + *pos + 1, 4), true, slot, stack,
                                      depth, err);
    default: // 0xc1 (never used) and ext (0xc7-0xc9, 0xd4-0xd8): not modeled
      api_set_error(err, kErrorTypeValidation, "msgpack: unsupported type 0x%02x", b);
      return kMsgpackError;
  }
}

// Decodes one value's header at *pos into *slot: a scalar is filled completely; a
// container is initialized (Nil slots) and its frame pushed for the caller to
// fill. Reports incomplete/error without leaving a half-built scalar.
static MsgpackStatus mp_read_into(const uint8_t *buf, size_t len, size_t *pos, Object *slot,
                                  MpDecFrame *stack, int *depth, Error *err)
{
  if (len - *pos < 1) {
    return kMsgpackIncomplete;
  }
  uint8_t b = buf[*pos];
  if (b <= 0x7f) {
    *slot = INTEGER_OBJ(b); // positive fixint
    *pos += 1;
    return kMsgpackOk;
  }
  if (b >= 0xe0) {
    *slot = INTEGER_OBJ((int8_t)b); // negative fixint
    *pos += 1;
    return kMsgpackOk;
  }
  if (b >= 0x80 && b <= 0x8f) {
    return mp_open(len, pos, 1, b & 0x0f, true, slot, stack, depth, err); // fixmap
  }
  if (b >= 0x90 && b <= 0x9f) {
    return mp_open(len, pos, 1, b & 0x0f, false, slot, stack, depth, err); // fixarray
  }
  if (b >= 0xa0 && b <= 0xbf) {
    return mp_take_str(buf, len, pos, 1, b & 0x1f, slot); // fixstr
  }
  if (b == 0xc0) {
    *slot = NIL;
    *pos += 1;
    return kMsgpackOk;
  }
  if (b == 0xc2 || b == 0xc3) {
    *slot = BOOLEAN_OBJ(b == 0xc3);
    *pos += 1;
    return kMsgpackOk;
  }
  return mp_read_ext(b, buf, len, pos, slot, stack, depth, err);
}

// Reads a map key, which must be a string (Object Dict keys are Strings).
static MsgpackStatus mp_read_key(const uint8_t *buf, size_t len, size_t *pos, String *key,
                                 Error *err)
{
  if (len - *pos < 1) {
    return kMsgpackIncomplete;
  }
  uint8_t b = buf[*pos];
  Object tmp = NIL;
  MsgpackStatus s;
  if (b >= 0xa0 && b <= 0xbf) {
    s = mp_take_str(buf, len, pos, 1, b & 0x1f, &tmp);
  } else if (b == 0xd9) {
    s = len - *pos < 2 ? kMsgpackIncomplete : mp_take_str(buf, len, pos, 2, buf[*pos + 1], &tmp);
  } else if (b == 0xda) {
    s = len - *pos < 3 ? kMsgpackIncomplete
                       : mp_take_str(buf, len, pos, 3, mp_be(buf + *pos + 1, 2), &tmp);
  } else if (b == 0xdb) {
    s = len - *pos < 5 ? kMsgpackIncomplete
                       : mp_take_str(buf, len, pos, 5, mp_be(buf + *pos + 1, 4), &tmp);
  } else {
    api_set_error(err, kErrorTypeValidation, "msgpack: map key must be a string");
    return kMsgpackError;
  }
  if (s == kMsgpackOk) {
    *key = tmp.data.string;
  }
  return s;
}

MsgpackStatus msgpack_to_object(const uint8_t *buf, size_t len, size_t *consumed, Object *out,
                                Error *err)
{
  MpDecFrame stack[kMarshalDepthCap];
  int depth = 0;
  size_t pos = 0;
  *out = NIL; // so api_free_object is safe on any partial failure
  MsgpackStatus st = mp_read_into(buf, len, &pos, out, stack, &depth, err);
  while (st == kMsgpackOk && depth > 0) {
    MpDecFrame *f = &stack[depth - 1];
    if (f->idx >= f->remaining) {
      depth--;
      continue;
    }
    if (f->is_map && !f->awaiting_value) {
      st = mp_read_key(buf, len, &pos, &f->container->data.dict.items[f->idx].key, err);
      f->awaiting_value = true;
      continue;
    }
    Object *slot;
    if (f->is_map) {
      slot = &f->container->data.dict.items[f->idx].value;
      f->idx++;
      f->awaiting_value = false;
    } else {
      slot = &f->container->data.array.items[f->idx];
      f->idx++;
    }
    st = mp_read_into(buf, len, &pos, slot, stack, &depth, err);
  }
  if (st != kMsgpackOk) {
    api_free_object(out);
    *out = NIL;
    return st;
  }
  *consumed = pos;
  return kMsgpackOk;
}
