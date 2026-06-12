#include "mua/provider/openrouter.h"

#include <stdlib.h>
#include <string.h>

#include <cjson/cJSON.h>
#include <curl/curl.h>
#include <uv.h>

#include "mua/api/private/helpers.h"
#include "mua/json.h"
#include "mua/log.h"
#include "mua/memory.h"
#include "mua/provider/openrouter_internal.h"
#include "mua/sse.h"

enum {
  kOrEventJsonCap = MUA_SSE_MAX_EVENT_DATA, // anything the SSE caps let through
  kOrErrorBodyCap = 64 * 1024,
  kOrFinishReasonCap = 32,
  kOrBackoffBaseMs = 500,
  kOrBackoffCapMs = 30000,
  kOrRetryAfterCapS = 60,
  kOrToolIdCap = 64,           // bytes, including the NUL
  kOrToolNameCap = 128,        // bytes, including the NUL
  kOrToolArgsCap = 256 * 1024, // concatenated arguments fragments per call
  kOrContentCap = 1024 * 1024, // full accumulated assistant text
  kOrToolItemsPerEvent = 64,   // delta.tool_calls items in one event
};

typedef enum {
  kOrModeUndecided = 0,
  kOrModeSse,
  kOrModeErrorBody,
} OrMode;

// One streamed tool call, accumulated fragment by fragment: id/name latch
// from the first fragment that carries them, arguments concatenate per index.
typedef struct {
  bool present; // some fragment named this index
  char id[kOrToolIdCap];
  char name[kOrToolNameCap];
  Buf args; // the arguments JSON string, possibly split across fragments
} OrToolCall;

struct OpenrouterStream {
  HttpClient *http;
  OpenrouterCallbacks cb;
  void *ud;
  char *url;         // owned
  char *auth_header; // owned; contains the key — zeroed before free
  String body;       // owned (json_print)
  SseParser *sse;
  Buf error_body;
  bool error_body_truncated;
  uv_timer_t retry_timer;
  bool timer_initialized;
  HttpRequest *req; // live attempt, or NULL
  int attempt;      // attempts started
  OrMode mode;
  bool wrong_ctype;
  long http_status;
  bool delivered; // some on_text fired: retrying could duplicate output
  bool finished;  // a terminal callback fired
  bool cancel_requested;
  char finish_reason[kOrFinishReasonCap];
  int64_t completion_tokens;
  Buf content; // full assistant text (on_text still streams deltas live)
  OrToolCall tool_calls[MUA_MAX_TOOL_CALLS];
  int tool_calls_hwm; // max present index + 1; assembly iterates [0, hwm)
  uint64_t prng_state;
};

static void accumulators_init(OpenrouterStream *stream)
{
  buf_init(&stream->content, kOrContentCap);
  for (int i = 0; i < MUA_MAX_TOOL_CALLS; i++) {
    buf_init(&stream->tool_calls[i].args, kOrToolArgsCap);
  }
}

static void accumulators_free(OpenrouterStream *stream)
{
  buf_free(&stream->content);
  for (int i = 0; i < MUA_MAX_TOOL_CALLS; i++) {
    buf_free(&stream->tool_calls[i].args);
  }
}

static void fire_error_msg(OpenrouterStream *stream, ErrorType type, const char *msg)
{
  if (stream->finished) {
    return;
  }
  stream->finished = true;
  Error err = ERROR_INIT;
  api_set_error(&err, type, "%s", msg);
  if (stream->cb.on_error != NULL) {
    stream->cb.on_error(stream->ud, &err);
  }
  api_clear_error(&err);
}

// Assembles the complete wire-shaped assistant message from the accumulators.
// Caller (fire_done) has already validated that present calls are complete.
static cJSON *assemble_message(OpenrouterStream *stream)
{
  cJSON *msg = json_new_obj();
  json_add_cstr(msg, "role", "assistant");
  if (stream->content.size > 0) {
    json_add_str(msg, "content",
                 (String){.data = stream->content.data, .size = stream->content.size});
  } else {
    cJSON_AddItemToObject(msg, "content", cJSON_CreateNull());
  }
  bool any = false;
  for (int i = 0; i < stream->tool_calls_hwm; i++) {
    any = any || stream->tool_calls[i].present;
  }
  if (!any) {
    return msg; // content-only response: no tool_calls key at all
  }
  cJSON *calls = json_add_arr(msg, "tool_calls");
  for (int i = 0; i < stream->tool_calls_hwm; i++) {
    OrToolCall *call = &stream->tool_calls[i];
    if (!call->present) {
      log_msg(kLogWarn, "openrouter: tool_call index gap at %d; skipping", i);
      continue;
    }
    cJSON *entry = cJSON_CreateObject();
    json_add_cstr(entry, "id", call->id);
    json_add_cstr(entry, "type", "function");
    cJSON *function = json_new_obj();
    json_add_cstr(function, "name", call->name);
    json_add_str(function, "arguments", (String){.data = call->args.data, .size = call->args.size});
    cJSON_AddItemToObject(entry, "function", function);
    cJSON_AddItemToArray(calls, entry);
  }
  return msg;
}

static void fire_done(OpenrouterStream *stream)
{
  if (stream->finished) {
    return;
  }
  for (int i = 0; i < stream->tool_calls_hwm; i++) {
    OrToolCall *call = &stream->tool_calls[i];
    if (call->present && (call->id[0] == '\0' || call->name[0] == '\0')) {
      // An assembled-but-incomplete call would corrupt the session replay.
      fire_error_msg(stream, kErrorTypeException, "incomplete tool call in stream");
      return;
    }
  }
  stream->finished = true;
  String reason = cstr_as_string(stream->finish_reason);
  if (stream->cb.on_done != NULL) {
    cJSON *msg = assemble_message(stream); // ownership transfers to the callee
    stream->cb.on_done(stream->ud, msg, &reason, stream->completion_tokens);
  }
}

// Folds one delta.tool_calls array into the per-index accumulators. Returns
// false after firing a terminal error: caps are never silently truncated (a
// truncated assembled message would corrupt the session).
static bool accumulate_tool_calls(OpenrouterStream *stream, const cJSON *items)
{
  int count = 0;
  for (const cJSON *item = items->child; item != NULL; item = item->next) {
    if (++count > kOrToolItemsPerEvent) {
      fire_error_msg(stream, kErrorTypeException, "too many tool_call items in one event");
      return false;
    }
    int64_t index = -1;
    if (!json_get_int(item, "index", &index) || index < 0 || index >= MUA_MAX_TOOL_CALLS) {
      fire_error_msg(stream, kErrorTypeException, "tool_call index missing or out of range");
      return false;
    }
    OrToolCall *call = &stream->tool_calls[index];
    call->present = true;
    if ((int)index + 1 > stream->tool_calls_hwm) {
      stream->tool_calls_hwm = (int)index + 1;
    }
    stream->delivered = true; // the model began answering; a retry could re-bill
    const char *id = json_get_cstr(item, "id");
    if (id != NULL && id[0] != '\0' && call->id[0] == '\0') { // latch the first fragment's
      if (strlen(id) >= sizeof(call->id)) {
        fire_error_msg(stream, kErrorTypeException, "tool_call id exceeds cap");
        return false;
      }
      memcpy(call->id, id, strlen(id) + 1);
    }
    const cJSON *function = json_get_obj(item, "function");
    if (function == NULL) {
      continue; // a fragment may carry only index/id
    }
    const char *name = json_get_cstr(function, "name");
    if (name != NULL && name[0] != '\0' && call->name[0] == '\0') {
      if (strlen(name) >= sizeof(call->name)) {
        fire_error_msg(stream, kErrorTypeException, "tool_call name exceeds cap");
        return false;
      }
      memcpy(call->name, name, strlen(name) + 1);
    }
    const char *args = json_get_cstr(function, "arguments");
    if (args != NULL && args[0] != '\0' && !buf_append(&call->args, args, strlen(args))) {
      fire_error_msg(stream, kErrorTypeException, "tool_call arguments exceed cap");
      return false;
    }
  }
  return true;
}

bool openrouter_handle_event(OpenrouterStream *stream, String data)
{
  if (stream->finished) {
    return false;
  }
  if (data.size == 6 && memcmp(data.data, "[DONE]", 6) == 0) {
    fire_done(stream);
    return false; // nothing meaningful follows the sentinel
  }
  Error err = ERROR_INIT;
  cJSON *doc = json_parse(data, kOrEventJsonCap, &err);
  if (doc == NULL) {
    fire_error_msg(stream, kErrorTypeException,
                   err.msg != NULL ? err.msg : "malformed stream event");
    api_clear_error(&err);
    return false;
  }
  cJSON *error_obj = json_get_obj(doc, "error");
  if (error_obj != NULL) { // mid-stream errors arrive as data chunks
    const char *message = json_get_cstr(error_obj, "message");
    Error api_err = ERROR_INIT;
    api_set_error(&api_err, kErrorTypeException, "api error: %s",
                  message != NULL ? message : "(no message)");
    stream->finished = true;
    if (stream->cb.on_error != NULL) {
      stream->cb.on_error(stream->ud, &api_err);
    }
    api_clear_error(&api_err);
    json_free(doc);
    return false;
  }
  cJSON *choices = json_get_arr(doc, "choices");
  cJSON *choice = (choices != NULL) ? cJSON_GetArrayItem(choices, 0) : NULL;
  if (choice != NULL) {
    cJSON *delta = json_get_obj(choice, "delta");
    const char *content = (delta != NULL) ? json_get_cstr(delta, "content") : NULL;
    if (content != NULL && content[0] != '\0') {
      if (!buf_append(&stream->content, content, strlen(content))) {
        fire_error_msg(stream, kErrorTypeException, "assistant content exceeds cap");
        json_free(doc);
        return false;
      }
      stream->delivered = true;
      if (stream->cb.on_text != NULL) {
        String text = cstr_as_string(content);
        stream->cb.on_text(stream->ud, &text);
      }
    }
    cJSON *calls = (delta != NULL) ? json_get_arr(delta, "tool_calls") : NULL;
    if (calls != NULL && !accumulate_tool_calls(stream, calls)) {
      json_free(doc);
      return false;
    }
    const char *finish = json_get_cstr(choice, "finish_reason");
    if (finish != NULL) {
      // Bounded copy; real values ("stop", "length", ...) are short.
      size_t len = strnlen(finish, sizeof(stream->finish_reason) - 1);
      memcpy(stream->finish_reason, finish, len);
      stream->finish_reason[len] = '\0';
    }
  }
  cJSON *usage = json_get_obj(doc, "usage");
  if (usage != NULL) {
    // Liberal acceptance: absent/mistyped usage simply leaves the latch.
    (void)json_get_int(usage, "completion_tokens", &stream->completion_tokens);
  }
  json_free(doc);
  return true;
}

static bool or_on_event(void *ud, const String *event_type, const String *data, const String *id)
{
  (void)event_type; // OpenRouter streams bare data: lines
  (void)id;
  return openrouter_handle_event(ud, *data);
}

static void or_on_status(void *ud, long status, const char *content_type)
{
  OpenrouterStream *stream = ud;
  stream->http_status = status;
  if (status == 200) {
    // Parameters like "; charset=utf-8" are irrelevant; match the prefix.
    if (content_type != NULL && strncmp(content_type, "text/event-stream", 17) == 0) {
      stream->mode = kOrModeSse;
    } else {
      stream->mode = kOrModeErrorBody;
      stream->wrong_ctype = true;
    }
  } else {
    stream->mode = kOrModeErrorBody;
  }
}

static bool or_on_chunk(void *ud, const char *bytes, size_t len)
{
  OpenrouterStream *stream = ud;
  if (stream->mode == kOrModeSse) {
    Error err = ERROR_INIT;
    if (!sse_parser_feed(stream->sse, bytes, len, &err)) {
      // Either the parser overflowed a cap (report it) or the event callback
      // already fired a terminal callback (finished is set; stay quiet).
      fire_error_msg(stream, kErrorTypeException,
                     err.msg != NULL ? err.msg : "stream parse failure");
      api_clear_error(&err);
      return false;
    }
    return true;
  }
  // Error-body mode: accumulate the whole (bounded) body; note truncation.
  if (!buf_append(&stream->error_body, bytes, len)) {
    stream->error_body_truncated = true;
  }
  return true;
}

static uint64_t prng_next(OpenrouterStream *stream)
{
  uint64_t x = stream->prng_state;
  x ^= x << 13;
  x ^= x >> 7;
  x ^= x << 17;
  stream->prng_state = x;
  return x;
}

static bool status_is_retryable(long status)
{
  // 402 (payment required) and other 4xx are terminal by design.
  return status == 408 || status == 429 || status == 500 || status == 502 || status == 503 ||
         status == 504;
}

static bool curl_code_is_retryable(CURLcode code)
{
  return code == CURLE_COULDNT_RESOLVE_HOST || code == CURLE_COULDNT_CONNECT ||
         code == CURLE_SSL_CONNECT_ERROR || code == CURLE_SEND_ERROR || code == CURLE_RECV_ERROR ||
         code == CURLE_OPERATION_TIMEDOUT || code == CURLE_GOT_NOTHING || code == CURLE_HTTP2 ||
         code == CURLE_HTTP2_STREAM;
}

static void stream_destroy(OpenrouterStream *stream);
static void start_attempt(OpenrouterStream *stream);

static void retry_timer_cb(uv_timer_t *handle)
{
  start_attempt(handle->data);
}

static void schedule_retry(OpenrouterStream *stream, long retry_after_s)
{
  long shift = stream->attempt < 6 ? stream->attempt : 6; // 500ms << 6 caps the math
  long delay_ms = kOrBackoffBaseMs << shift;
  if (delay_ms > kOrBackoffCapMs) {
    delay_ms = kOrBackoffCapMs;
  }
  // Full jitter: [delay/2, delay].
  delay_ms = (delay_ms / 2) + (long)(prng_next(stream) % (uint64_t)((delay_ms / 2) + 1));
  if (retry_after_s >= 0) {
    long ra_ms = (retry_after_s < kOrRetryAfterCapS ? retry_after_s : kOrRetryAfterCapS) * 1000;
    if (ra_ms > delay_ms) {
      delay_ms = ra_ms;
    }
  }
  log_msg(kLogInfo, "openrouter: retrying in %ld ms (attempt %d of %d)", delay_ms,
          stream->attempt + 1, MUA_HTTP_MAX_ATTEMPTS);
  if (uv_timer_start(&stream->retry_timer, retry_timer_cb, (uint64_t)delay_ms, 0) != 0) {
    fire_error_msg(stream, kErrorTypeException, "failed to schedule a retry");
    stream_destroy(stream);
  }
}

// Builds the user-facing failure message for a finished error-body response.
static void report_http_failure(OpenrouterStream *stream)
{
  if (stream->wrong_ctype) {
    fire_error_msg(stream, kErrorTypeException, "unexpected content-type on HTTP 200 response");
    return;
  }
  String body = (String){.data = stream->error_body.data, .size = stream->error_body.size};
  Error parse_err = ERROR_INIT;
  cJSON *doc = (body.size > 0) ? json_parse(body, kOrErrorBodyCap, &parse_err) : NULL;
  api_clear_error(&parse_err); // an unparseable error body is not itself an error
  const char *message = NULL;
  if (doc != NULL) {
    cJSON *error_obj = json_get_obj(doc, "error");
    message = (error_obj != NULL) ? json_get_cstr(error_obj, "message") : NULL;
  }
  Error err = ERROR_INIT;
  if (message != NULL) {
    api_set_error(&err, kErrorTypeException, "api error (HTTP %ld): %s%s", stream->http_status,
                  message, stream->error_body_truncated ? " [truncated]" : "");
  } else {
    api_set_error(&err, kErrorTypeException, "api error (HTTP %ld)", stream->http_status);
  }
  if (!stream->finished) {
    stream->finished = true;
    if (stream->cb.on_error != NULL) {
      stream->cb.on_error(stream->ud, &err);
    }
  }
  api_clear_error(&err);
  json_free(doc);
}

static void or_on_complete(void *ud, CURLcode result, long http_status, const char *errmsg,
                           long retry_after_s)
{
  OpenrouterStream *stream = ud;
  stream->req = NULL;
  if (stream->finished) { // terminal already fired mid-stream ([DONE], error event, ...)
    stream_destroy(stream);
    return;
  }
  if (stream->cancel_requested) {
    fire_error_msg(stream, kErrorTypeException, "canceled");
    stream_destroy(stream);
    return;
  }
  bool retryable = false;
  if (stream->mode == kOrModeSse && result == CURLE_OK) {
    if (stream->finish_reason[0] != '\0' && sse_parser_finish(stream->sse) == kSseEofClean) {
      // Tolerate a dropped [DONE]: the stream closed cleanly after a
      // finish_reason — some proxies eat the sentinel.
      fire_done(stream);
      stream_destroy(stream);
      return;
    }
    fire_error_msg(stream, kErrorTypeException, "stream ended before completion");
    stream_destroy(stream);
    return;
  }
  if (stream->mode == kOrModeErrorBody || stream->mode == kOrModeUndecided) {
    retryable = status_is_retryable(http_status != 0 ? http_status : stream->http_status);
  }
  if (result != CURLE_OK) {
    retryable = retryable || curl_code_is_retryable(result);
  }
  if (retryable && !stream->delivered && stream->attempt < MUA_HTTP_MAX_ATTEMPTS) {
    schedule_retry(stream, retry_after_s);
    return;
  }
  if (stream->mode == kOrModeErrorBody) {
    report_http_failure(stream);
  } else {
    Error err = ERROR_INIT;
    api_set_error(&err, kErrorTypeException, "request failed: %s",
                  errmsg != NULL ? errmsg : curl_easy_strerror(result));
    stream->finished = true;
    if (stream->cb.on_error != NULL) {
      stream->cb.on_error(stream->ud, &err);
    }
    api_clear_error(&err);
  }
  stream_destroy(stream);
}

static void start_attempt(OpenrouterStream *stream)
{
  sse_parser_reset(stream->sse);
  buf_reset(&stream->error_body);
  stream->error_body_truncated = false;
  stream->mode = kOrModeUndecided;
  stream->wrong_ctype = false;
  stream->http_status = 0;
  // Full per-attempt reset: a retry must never inherit a prior attempt's
  // latches or accumulators. `delivered` deliberately persists — it gates
  // whether a retry is allowed at all.
  stream->finish_reason[0] = '\0';
  stream->completion_tokens = 0;
  buf_reset(&stream->content);
  for (int i = 0; i < MUA_MAX_TOOL_CALLS; i++) {
    stream->tool_calls[i].present = false;
    stream->tool_calls[i].id[0] = '\0';
    stream->tool_calls[i].name[0] = '\0';
    buf_reset(&stream->tool_calls[i].args);
  }
  stream->tool_calls_hwm = 0;
  stream->attempt++;
  const char *headers[] = {
    stream->auth_header,
    "Content-Type: application/json",
    "Accept: text/event-stream",
  };
  HttpRequestOpts opts = {
    .url = stream->url,
    .headers = headers,
    .header_count = sizeof(headers) / sizeof(headers[0]),
    .body = stream->body,
  };
  HttpCallbacks cb = {
    .on_status = or_on_status,
    .on_chunk = or_on_chunk,
    .on_complete = or_on_complete,
  };
  Error err = ERROR_INIT;
  stream->req = http_request(stream->http, &opts, &cb, stream, &err);
  if (stream->req == NULL) {
    fire_error_msg(stream, kErrorTypeException, err.msg != NULL ? err.msg : "request failed");
    api_clear_error(&err);
    stream_destroy(stream);
  }
}

static String build_body(const OpenrouterOpts *opts)
{
  cJSON *body = json_new_obj();
  json_add_cstr(body, "model", opts->model != NULL ? opts->model : MUA_DEFAULT_MODEL);
  json_add_bool(body, "stream", true);
  if (opts->max_tokens > 0) {
    json_add_int(body, "max_tokens", opts->max_tokens);
  }
  // Zero-copy request build: a reference array borrows the caller's items
  // (the array's first element, not the array node) for the print, and
  // json_free(body) skips reference children — verified, cJSON 1.7.18.
  cJSON_AddItemToObject(body, "messages", cJSON_CreateArrayReference(opts->messages->child));
  if (opts->tools != NULL && opts->tools->child != NULL) {
    cJSON_AddItemToObject(body, "tools", cJSON_CreateArrayReference(opts->tools->child));
  }
  String printed = json_print(body);
  json_free(body);
  return printed;
}

static char *build_url(const OpenrouterOpts *opts)
{
  const char *base = opts->base_url;
  if (base == NULL || base[0] == '\0') {
    base = getenv("OPENROUTER_BASE_URL");
  }
  if (base == NULL || base[0] == '\0') {
    base = MUA_OPENROUTER_BASE_URL;
  }
  size_t base_len = strlen(base);
  while (base_len > 0 && base[base_len - 1] == '/') {
    base_len--;
  }
  static const char path[] = "/chat/completions";
  char *url = xmalloc(base_len + sizeof(path));
  memcpy(url, base, base_len);
  memcpy(url + base_len, path, sizeof(path)); // includes the NUL
  return url;
}

static void retry_timer_closed(uv_handle_t *handle)
{
  OpenrouterStream *stream = handle->data;
  if (stream->auth_header != NULL) {
    // The bearer key lived here; do not leave it in freed heap pages.
    memset(stream->auth_header, 0, strlen(stream->auth_header));
  }
  xfree(stream->auth_header);
  xfree(stream->url);
  xfree(stream->body.data);
  sse_parser_free(stream->sse);
  buf_free(&stream->error_body);
  accumulators_free(stream);
  xfree(stream);
}

static void stream_destroy(OpenrouterStream *stream)
{
  if (stream->timer_initialized) {
    uv_close((uv_handle_t *)&stream->retry_timer, retry_timer_closed);
  } else {
    retry_timer_closed((uv_handle_t *)&stream->retry_timer);
  }
}

OpenrouterStream *openrouter_stream(HttpClient *http, const OpenrouterOpts *opts,
                                    const OpenrouterCallbacks *cb, void *ud, Error *err)
{
  if (opts->api_key == NULL || opts->api_key[0] == '\0') {
    api_set_error(err, kErrorTypeValidation, "openrouter: api key is required");
    return NULL;
  }
  // ->child == NULL would silently print "messages":[] and earn an API 400.
  if (!cJSON_IsArray(opts->messages) || opts->messages->child == NULL) {
    api_set_error(err, kErrorTypeValidation, "openrouter: messages must be a non-empty array");
    return NULL;
  }
  if (opts->tools != NULL && !cJSON_IsArray(opts->tools)) {
    api_set_error(err, kErrorTypeValidation, "openrouter: tools must be an array");
    return NULL;
  }
  OpenrouterStream *stream = xcalloc(1, sizeof(*stream));
  stream->http = http;
  stream->cb = *cb;
  stream->ud = ud;
  // Set before any destroy path: retry_timer_closed reads handle->data.
  stream->retry_timer.data = stream;
  stream->url = build_url(opts);
  stream->body = build_body(opts);
  static const char bearer_prefix[] = "Authorization: Bearer ";
  size_t key_len = strlen(opts->api_key);
  stream->auth_header = xmalloc(sizeof(bearer_prefix) + key_len);
  memcpy(stream->auth_header, bearer_prefix, sizeof(bearer_prefix) - 1);
  memcpy(stream->auth_header + sizeof(bearer_prefix) - 1, opts->api_key, key_len + 1);
  stream->sse = sse_parser_new(NULL, or_on_event, stream);
  buf_init(&stream->error_body, kOrErrorBodyCap);
  accumulators_init(stream);
  stream->prng_state = uv_hrtime() | 1U; // seeded once; never zero
  if (uv_timer_init(http_client_loop(http), &stream->retry_timer) != 0) {
    stream_destroy(stream);
    api_set_error(err, kErrorTypeException, "openrouter: timer init failed");
    return NULL;
  }
  stream->timer_initialized = true;
  start_attempt(stream);
  return stream;
}

void openrouter_cancel(OpenrouterStream *stream)
{
  if (stream == NULL || stream->finished || stream->cancel_requested) {
    return;
  }
  stream->cancel_requested = true;
  if (stream->req != NULL) {
    http_cancel(stream->req); // or_on_complete reports "canceled" and destroys
    return;
  }
  // Waiting on a retry: stop the timer and finish now.
  (void)uv_timer_stop(&stream->retry_timer); // cannot fail on an initialized timer
  fire_error_msg(stream, kErrorTypeException, "canceled");
  stream_destroy(stream);
}

OpenrouterStream *openrouter_stream_new_for_test(const OpenrouterCallbacks *cb, void *ud)
{
  OpenrouterStream *stream = xcalloc(1, sizeof(*stream));
  stream->cb = *cb;
  stream->ud = ud;
  accumulators_init(stream);
  return stream;
}

void openrouter_stream_free_for_test(OpenrouterStream *stream)
{
  accumulators_free(stream);
  xfree(stream);
}

bool openrouter_stream_delivered_for_test(const OpenrouterStream *stream)
{
  return stream->delivered;
}
