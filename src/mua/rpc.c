#include "mua/rpc.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <uv.h>

#include "mua/api/dispatch.h"
#include "mua/api/private/helpers.h"
#include "mua/log.h"
#include "mua/loop.h"
#include "mua/memory.h"
#include "mua/msgpack.h"

// A msgpack-RPC channel over stdio. The transport is libuv (uv_pipe_t on fd 0/1)
// so a SIGINT, handled by the loop, stops it cleanly; dispatch is synchronous
// (no model calls in this mode). Request [0, msgid, method, params] -> response
// [1, msgid, error, result]; notification [2, method, params] gets no reply.
// Untrusted input is bounded: the accumulation buffer is capped and the codec
// caps decode depth, so a malformed or oversized stream closes the channel.

enum {
  kRpcReadChunk = 65536,
  kRpcMaxMessage = 16 << 20, // 16 MiB: a single message past this closes the channel
};

typedef struct {
  uv_pipe_t in;
  uv_pipe_t out;
  uint8_t *buf; // accumulated, not-yet-decoded input bytes
  size_t len;
  size_t cap;
  bool stopping;
  uint8_t scratch[kRpcReadChunk]; // libuv reads into this; read_cb appends to buf
} RpcServer;

// A write request and the encoded bytes it owns (freed when the write completes).
typedef struct {
  uv_write_t req;
  MsgpackBuffer buf;
} WriteCtx;

static void on_write(uv_write_t *req, int status)
{
  (void)status; // even a cancelled write (channel closing) must free its buffer
  WriteCtx *w = (WriteCtx *)req;
  msgpack_buffer_free(&w->buf);
  xfree(w);
}

// Encodes [1, msgid, error, result] and queues it on stdout. The loop keeps the
// write alive until it flushes (or is cancelled on close); on_write frees it.
static void write_response(RpcServer *s, int64_t msgid, const Object *error, const Object *result)
{
  WriteCtx *w = xmalloc(sizeof *w);
  w->buf = MSGPACK_BUFFER_INIT;
  Object type_response = INTEGER_OBJ(1);
  Object id = INTEGER_OBJ(msgid);
  msgpack_encode_array_header(&w->buf, 4);
  msgpack_encode(&w->buf, &type_response);
  msgpack_encode(&w->buf, &id);
  msgpack_encode(&w->buf, error);
  msgpack_encode(&w->buf, result);
  uv_buf_t bytes = uv_buf_init((char *)w->buf.data, (unsigned int)w->buf.size);
  if (uv_write(&w->req, (uv_stream_t *)&s->out, &bytes, 1, on_write) != 0) {
    msgpack_buffer_free(&w->buf);
    xfree(w);
  }
}

// Dispatches one request and writes its response. An unknown method, non-array
// params, or an API error all yield an error response -- a clean, addressable
// failure, never a crash.
static void dispatch_and_respond(RpcServer *s, int64_t msgid, String method, const Object *params)
{
  Object result = NIL;
  Object error = NIL;
  Object err_items[2];
  Error err = ERROR_INIT;
  const ApiFnMeta *meta = api_dispatch_find(method.data, method.size);
  if (meta == NULL) {
    api_set_error(&err, kErrorTypeValidation, "unknown method: %.*s", (int)method.size,
                  method.data != NULL ? method.data : "");
  } else if (params->type != kObjectTypeArray) {
    api_set_error(&err, kErrorTypeValidation, "params must be an array");
  } else {
    result = meta->fn(params->data.array, &err);
  }
  if (ERROR_SET(&err)) {
    err_items[0] = INTEGER_OBJ(err.type);
    err_items[1] = STRING_OBJ(cstr_as_string(err.msg != NULL ? err.msg : "error"));
    error = ARRAY_OBJ(((Array){.items = err_items, .size = 2, .capacity = 2}));
  }
  write_response(s, msgid, &error, &result); // encodes now, borrowing err.msg
  api_free_object(&result);
  api_clear_error(&err);
}

// Dispatches a notification for its side effects; per msgpack-RPC it gets no
// reply, and a bad method or params is silently ignored.
static void dispatch_notify(String method, const Object *params)
{
  const ApiFnMeta *meta = api_dispatch_find(method.data, method.size);
  if (meta == NULL || params->type != kObjectTypeArray) {
    return;
  }
  Error err = ERROR_INIT;
  Object result = meta->fn(params->data.array, &err);
  api_free_object(&result);
  api_clear_error(&err);
}

static void begin_stop(RpcServer *s)
{
  if (s->stopping) {
    return;
  }
  s->stopping = true;
  uv_read_stop((uv_stream_t *)&s->in);
  uv_close((uv_handle_t *)&s->in, NULL); // queued writes still flush; then the loop idles
}

// Routes one decoded frame. A structurally-malformed top-level frame is a
// protocol violation and closes the channel; bad method/params within a
// well-formed request is answered with an error response instead.
static void handle_frame(RpcServer *s, const Object *frame)
{
  if (frame->type != kObjectTypeArray) {
    log_msg(kLogWarn, "rpc: frame is not an array; closing");
    begin_stop(s);
    return;
  }
  Array a = frame->data.array;
  bool is_request = a.size == 4 && a.items[0].type == kObjectTypeInteger &&
                    a.items[0].data.integer == 0;
  bool is_notify = a.size == 3 && a.items[0].type == kObjectTypeInteger &&
                   a.items[0].data.integer == 2;
  if (is_request) {
    if (a.items[1].type != kObjectTypeInteger || a.items[2].type != kObjectTypeString) {
      log_msg(kLogWarn, "rpc: malformed request header; closing");
      begin_stop(s);
      return;
    }
    dispatch_and_respond(s, a.items[1].data.integer, a.items[2].data.string, &a.items[3]);
  } else if (is_notify) {
    if (a.items[1].type != kObjectTypeString) {
      log_msg(kLogWarn, "rpc: malformed notification; closing");
      begin_stop(s);
      return;
    }
    dispatch_notify(a.items[1].data.string, &a.items[2]);
  } else {
    log_msg(kLogWarn, "rpc: not a request or notification; closing");
    begin_stop(s);
  }
}

static void rpc_append(RpcServer *s, const uint8_t *data, size_t n)
{
  if (s->len + n > s->cap) {
    size_t cap = s->cap != 0 ? s->cap : kRpcReadChunk;
    while (cap < s->len + n) {
      cap *= 2;
    }
    s->buf = xrealloc(s->buf, cap);
    s->cap = cap;
  }
  memcpy(s->buf + s->len, data, n);
  s->len += n;
}

// Pulls every complete frame out of the buffer, then compacts the remainder.
static void rpc_drain(RpcServer *s)
{
  while (!s->stopping && s->len > 0) {
    size_t consumed = 0;
    Object frame = NIL;
    Error err = ERROR_INIT;
    MsgpackStatus st = msgpack_to_object(s->buf, s->len, &consumed, &frame, &err);
    if (st == kMsgpackIncomplete) {
      api_clear_error(&err);
      if (s->len > (size_t)kRpcMaxMessage) {
        log_msg(kLogWarn, "rpc: message exceeds %d bytes; closing", kRpcMaxMessage);
        begin_stop(s);
      }
      return; // wait for more bytes
    }
    if (st == kMsgpackError) {
      log_msg(kLogWarn, "rpc: %s; closing", err.msg != NULL ? err.msg : "malformed message");
      api_clear_error(&err);
      begin_stop(s);
      return;
    }
    handle_frame(s, &frame);
    api_free_object(&frame);
    memmove(s->buf, s->buf + consumed, s->len - consumed);
    s->len -= consumed;
  }
}

static void alloc_cb(uv_handle_t *handle, size_t suggested, uv_buf_t *buf)
{
  (void)suggested;
  RpcServer *s = handle->data;
  *buf = uv_buf_init((char *)s->scratch, (unsigned int)sizeof s->scratch);
}

static void read_cb(uv_stream_t *stream, ssize_t nread, const uv_buf_t *buf)
{
  RpcServer *s = ((uv_handle_t *)stream)->data;
  if (nread < 0) { // UV_EOF or a read error: stop serving
    begin_stop(s);
    return;
  }
  if (nread > 0) {
    rpc_append(s, (const uint8_t *)buf->base, (size_t)nread);
    rpc_drain(s);
  }
}

// Closes a handle if it is not already closing, so the teardown is idempotent
// across the EOF path (handles already closing) and the SIGINT path (still open).
static void close_if_open(uv_handle_t *handle)
{
  if (!uv_is_closing(handle)) {
    uv_close(handle, NULL);
  }
}

int rpc_serve(void)
{
  RpcServer server = {0};
  uv_loop_t *loop = loop_get();
  if (uv_pipe_init(loop, &server.in, 0) != 0) {
    (void)fprintf(stderr, "mua: --embed: stdin pipe init failed\n");
    return 1;
  }
  if (uv_pipe_init(loop, &server.out, 0) != 0) {
    (void)fprintf(stderr, "mua: --embed: stdout pipe init failed\n");
    uv_close((uv_handle_t *)&server.in, NULL);
    (void)loop_run(); // process the close while `server` is still in scope
    return 1;
  }
  server.in.data = &server;
  server.out.data = &server;
  if (uv_pipe_open(&server.in, 0) != 0 || uv_pipe_open(&server.out, 1) != 0 ||
      uv_read_start((uv_stream_t *)&server.in, alloc_cb, read_cb) != 0) {
    (void)fprintf(stderr, "mua: --embed: cannot attach to stdio\n");
    close_if_open((uv_handle_t *)&server.in);
    close_if_open((uv_handle_t *)&server.out);
    (void)loop_run();
    xfree(server.buf);
    return 1;
  }
  (void)loop_run(); // serve until stdin EOF, or a SIGINT stops the loop
  close_if_open((uv_handle_t *)&server.in);
  close_if_open((uv_handle_t *)&server.out);
  (void)loop_run(); // drain handle closes before the stack frame goes away
  xfree(server.buf);
  return 0;
}
