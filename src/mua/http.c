#include "mua/http.h"

#include <stdlib.h>
#include <string.h>

#include <curl/curl.h>
#include <uv.h>

#include "mua/api/private/helpers.h"
#include "mua/log.h"
#include "mua/memory.h"

struct HttpClient {
  uv_loop_t *loop;
  CURLM *multi;
  uv_timer_t timer;      // curl's single timeout timer
  uv_timer_t reap_timer; // 0-ms deferred work: cancellations requested mid-callback
  int active;            // live easy handles
  unsigned timers_open;  // uv handles still open (teardown bookkeeping)
  bool closing;
  HttpRequest *requests; // intrusive list of live requests
};

struct HttpRequest {
  CURL *easy;
  struct curl_slist *headers;
  HttpClient *client;
  HttpCallbacks cb;
  void *ud;
  char errbuf[CURL_ERROR_SIZE];
  bool status_reported;
  bool cancel_requested;
  HttpRequest *next;
};

typedef struct {
  uv_poll_t poll;
  curl_socket_t fd;
  HttpClient *client;
} SockCtx;

// Documented init-once latch: curl_global_init must run exactly once per
// process, before any other libcurl call.
static bool g_curl_initialized = false;

static void report_status(HttpRequest *req)
{
  long status = 0;
  if (curl_easy_getinfo(req->easy, CURLINFO_RESPONSE_CODE, &status) != CURLE_OK) {
    status = 0;
  }
  char *content_type = NULL;
  if (curl_easy_getinfo(req->easy, CURLINFO_CONTENT_TYPE, &content_type) != CURLE_OK) {
    content_type = NULL;
  }
  req->status_reported = true;
  if (req->cb.on_status != NULL) {
    req->cb.on_status(req->ud, status, content_type);
  }
}

static size_t write_cb(char *ptr, size_t size, size_t nmemb, void *userdata)
{
  HttpRequest *req = userdata;
  size_t total = size * nmemb; // bounded by CURL_MAX_WRITE_SIZE per call
  if (req->cancel_requested) {
    return CURL_WRITEFUNC_ERROR; // fast-path cancel; the reap covers idle transfers
  }
  if (!req->status_reported) {
    report_status(req);
  }
  if (req->cb.on_chunk != NULL && total > 0) {
    if (!req->cb.on_chunk(req->ud, ptr, total)) {
      return CURL_WRITEFUNC_ERROR;
    }
  }
  return total;
}

static long get_retry_after(CURL *easy)
{
  struct curl_header *header = NULL;
  if (curl_easy_header(easy, "Retry-After", 0, CURLH_HEADER, -1, &header) != CURLHE_OK) {
    return -1;
  }
  char *end = NULL;
  long value = strtol(header->value, &end, 10);
  if (end == header->value || value < 0) {
    return -1; // HTTP-date form (or garbage): treat as absent
  }
  return value;
}

static void unlink_request(HttpClient *client, HttpRequest *req)
{
  if (client->requests == req) {
    client->requests = req->next;
    return;
  }
  HttpRequest *prev = client->requests;
  // Bounded by the number of live requests.
  while (prev != NULL && prev->next != req) {
    prev = prev->next;
  }
  if (prev != NULL) {
    prev->next = req->next;
  }
}

// The single place a request dies. Never called from inside a libcurl
// callback: only from check_multi_info, the reap timer, or client close.
static void finish_request(HttpClient *client, HttpRequest *req, CURLcode result)
{
  long status = 0;
  if (curl_easy_getinfo(req->easy, CURLINFO_RESPONSE_CODE, &status) != CURLE_OK) {
    status = 0;
  }
  if (!req->status_reported && status > 0) {
    report_status(req); // bodyless response: report before completion
  }
  long retry_after = get_retry_after(req->easy);
  const char *errmsg = (req->errbuf[0] != '\0') ? req->errbuf : curl_easy_strerror(result);
  unlink_request(client, req);
  CURLMcode mc = curl_multi_remove_handle(client->multi, req->easy);
  if (mc != CURLM_OK) {
    log_msg(kLogError, "curl_multi_remove_handle: %s", curl_multi_strerror(mc));
  }
  client->active--;
  if (req->cb.on_complete != NULL) {
    req->cb.on_complete(req->ud, result, status, errmsg, retry_after);
  }
  curl_slist_free_all(req->headers);
  curl_easy_cleanup(req->easy);
  xfree(req);
}

static void check_multi_info(HttpClient *client)
{
  int drained = 0;
  int cap = client->active + 1;
  CURLMsg *msg = NULL;
  int pending = 0;
  while ((msg = curl_multi_info_read(client->multi, &pending)) != NULL) {
    if (++drained > cap) {
      break; // can't happen: more DONE messages than live handles
    }
    if (msg->msg != CURLMSG_DONE) {
      continue;
    }
    // Copy out before any further multi calls invalidate msg.
    CURL *easy = msg->easy_handle;
    CURLcode result = msg->data.result;
    HttpRequest *req = NULL;
    if (curl_easy_getinfo(easy, CURLINFO_PRIVATE, &req) != CURLE_OK || req == NULL) {
      log_msg(kLogError, "http: DONE for unknown easy handle");
      continue;
    }
    finish_request(client, req, result);
  }
}

static void do_socket_action(HttpClient *client, curl_socket_t fd, int ev_bitmask)
{
  int running = 0;
  CURLMcode mc = curl_multi_socket_action(client->multi, fd, ev_bitmask, &running);
  if (mc != CURLM_OK) {
    log_msg(kLogError, "curl_multi_socket_action: %s", curl_multi_strerror(mc));
  }
  // Completions are processed only after socket_action returns — never
  // re-enter curl from inside its own callback stack.
  check_multi_info(client);
}

static void poll_cb(uv_poll_t *handle, int status, int events)
{
  SockCtx *ctx = handle->data;
  int flags = 0;
  if (status < 0) {
    flags = CURL_CSELECT_ERR; // historically dropped by multi-uv.c; do not
  } else {
    if ((events & UV_READABLE) != 0) {
      flags |= CURL_CSELECT_IN;
    }
    if ((events & UV_WRITABLE) != 0) {
      flags |= CURL_CSELECT_OUT;
    }
  }
  do_socket_action(ctx->client, ctx->fd, flags);
}

static void on_timeout(uv_timer_t *handle)
{
  HttpClient *client = handle->data;
  do_socket_action(client, CURL_SOCKET_TIMEOUT, 0);
}

static int timer_function(CURLM *multi, long timeout_ms, void *userp)
{
  (void)multi;
  HttpClient *client = userp;
  if (timeout_ms < 0) {
    (void)uv_timer_stop(&client->timer); // cannot fail on an initialized timer
    return 0;
  }
  // timeout_ms == 0 defers to the next loop turn: this callback may run
  // inside curl_multi_socket_action and must not re-enter curl.
  if (uv_timer_start(&client->timer, on_timeout, (uint64_t)timeout_ms, 0) != 0) {
    return -1;
  }
  return 0;
}

static void sockctx_close_cb(uv_handle_t *handle)
{
  SockCtx *ctx = handle->data;
  xfree(ctx);
}

static int socket_function(CURL *easy, curl_socket_t fd, int what, void *userp, void *socketp)
{
  (void)easy;
  HttpClient *client = userp;
  SockCtx *ctx = socketp;
  if (what == CURL_POLL_REMOVE) {
    if (ctx != NULL) {
      // Detach from curl FIRST: the fd may be closed and reused before the
      // deferred uv close lands; a re-registration gets a fresh context.
      (void)curl_multi_assign(client->multi, fd, NULL); // detaching can't fail meaningfully
      (void)uv_poll_stop(&ctx->poll);                   // stopping a stopped poll is a no-op
      uv_close((uv_handle_t *)&ctx->poll, sockctx_close_cb);
    }
    return 0;
  }
  if (ctx == NULL) {
    ctx = xmalloc(sizeof(*ctx));
    ctx->fd = fd;
    ctx->client = client;
    if (uv_poll_init_socket(client->loop, &ctx->poll, fd) != 0) {
      xfree(ctx);
      return -1;
    }
    ctx->poll.data = ctx;
    if (curl_multi_assign(client->multi, fd, ctx) != CURLM_OK) {
      uv_close((uv_handle_t *)&ctx->poll, sockctx_close_cb);
      return -1;
    }
  }
  int events = 0;
  if (what == CURL_POLL_IN || what == CURL_POLL_INOUT) {
    events |= UV_READABLE;
  }
  if (what == CURL_POLL_OUT || what == CURL_POLL_INOUT) {
    events |= UV_WRITABLE;
  }
  // uv_poll_start on a running handle replaces the mask (recomputed, never
  // accumulated).
  if (uv_poll_start(&ctx->poll, events, poll_cb) != 0) {
    return -1;
  }
  return 0;
}

static void reap_cb(uv_timer_t *handle)
{
  HttpClient *client = handle->data;
  HttpRequest *req = client->requests;
  // Bounded by the number of live requests; finish_request unlinks, and
  // on_complete may prepend new requests, neither of which disturbs `next`
  // captured before the call.
  while (req != NULL) {
    HttpRequest *next = req->next;
    if (req->cancel_requested) {
      finish_request(client, req, CURLE_ABORTED_BY_CALLBACK);
    }
    req = next;
  }
}

static void client_handle_closed(uv_handle_t *handle)
{
  HttpClient *client = handle->data;
  client->timers_open--;
  if (client->timers_open == 0) {
    xfree(client);
  }
}

HttpClient *http_client_new(uv_loop_t *loop, Error *err)
{
  if (!g_curl_initialized) {
    if (curl_global_init(CURL_GLOBAL_DEFAULT) != CURLE_OK) {
      api_set_error(err, kErrorTypeException, "http: curl_global_init failed");
      return NULL;
    }
    g_curl_initialized = true;
  }
  CURLM *multi = curl_multi_init();
  if (multi == NULL) {
    api_set_error(err, kErrorTypeException, "http: curl_multi_init failed");
    return NULL;
  }
  HttpClient *client = xcalloc(1, sizeof(*client));
  client->loop = loop;
  client->multi = multi;
  if (uv_timer_init(loop, &client->timer) != 0 || uv_timer_init(loop, &client->reap_timer) != 0) {
    // uv_timer_init on a live loop only fails on resource exhaustion.
    (void)curl_multi_cleanup(multi);
    xfree(client);
    api_set_error(err, kErrorTypeException, "http: timer init failed");
    return NULL;
  }
  client->timer.data = client;
  client->reap_timer.data = client;
  client->timers_open = 2;
  bool opts_ok = curl_multi_setopt(multi, CURLMOPT_SOCKETFUNCTION, socket_function) == CURLM_OK &&
                 curl_multi_setopt(multi, CURLMOPT_SOCKETDATA, client) == CURLM_OK &&
                 curl_multi_setopt(multi, CURLMOPT_TIMERFUNCTION, timer_function) == CURLM_OK &&
                 curl_multi_setopt(multi, CURLMOPT_TIMERDATA, client) == CURLM_OK;
  if (!opts_ok) {
    http_client_close(client);
    api_set_error(err, kErrorTypeException, "http: curl_multi_setopt failed");
    return NULL;
  }
  return client;
}

void http_client_close(HttpClient *client)
{
  if (client == NULL || client->closing) {
    return;
  }
  client->closing = true;
  // Finish every live request now: close runs from teardown, outside any
  // curl callback stack, so direct unwinding is legal. Bounded by `active`.
  while (client->requests != NULL) {
    finish_request(client, client->requests, CURLE_ABORTED_BY_CALLBACK);
  }
  CURLMcode mc = curl_multi_cleanup(client->multi);
  if (mc != CURLM_OK) {
    log_msg(kLogError, "curl_multi_cleanup: %s", curl_multi_strerror(mc));
  }
  uv_close((uv_handle_t *)&client->timer, client_handle_closed);
  uv_close((uv_handle_t *)&client->reap_timer, client_handle_closed);
}

uv_loop_t *http_client_loop(const HttpClient *client)
{
  return client->loop;
}

void http_global_cleanup(void)
{
  if (g_curl_initialized) {
    curl_global_cleanup();
    g_curl_initialized = false;
  }
}

// Transport-level options: timeouts, encodings, redirect policy.
static bool configure_transport(CURL *easy, const HttpRequestOpts *opts)
{
  long connect_ms =
    (opts->connect_timeout_ms > 0) ? opts->connect_timeout_ms : MUA_HTTP_CONNECT_TIMEOUT_MS;
  long stall_s = (opts->stall_window_s > 0) ? opts->stall_window_s : MUA_HTTP_STALL_WINDOW_S;
  // No CURLOPT_TIMEOUT: it would kill long streams. Stall detection instead.
  return curl_easy_setopt(easy, CURLOPT_CONNECTTIMEOUT_MS, connect_ms) == CURLE_OK &&
         curl_easy_setopt(easy, CURLOPT_LOW_SPEED_LIMIT, 1L) == CURLE_OK &&
         curl_easy_setopt(easy, CURLOPT_LOW_SPEED_TIME, stall_s) == CURLE_OK &&
         // curl inflates before the write callback; SSE never sees gzip bytes.
         curl_easy_setopt(easy, CURLOPT_ACCEPT_ENCODING, "") == CURLE_OK &&
         // Redirect-following with a bearer credential is needless attack
         // surface; the APIs we speak to never redirect.
         curl_easy_setopt(easy, CURLOPT_FOLLOWLOCATION, 0L) == CURLE_OK &&
         curl_easy_setopt(easy, CURLOPT_MAXREDIRS, 0L) == CURLE_OK &&
         curl_easy_setopt(easy, CURLOPT_NOSIGNAL, 1L) == CURLE_OK;
}

static bool configure_request(HttpRequest *req, const HttpRequestOpts *opts)
{
  CURL *easy = req->easy;
  const char *body = (opts->body.data != NULL) ? opts->body.data : "";
  curl_off_t body_size = (curl_off_t)opts->body.size;
  return curl_easy_setopt(easy, CURLOPT_URL, opts->url) == CURLE_OK &&
         curl_easy_setopt(easy, CURLOPT_POST, 1L) == CURLE_OK &&
         // Size first so NUL bytes in the body are preserved by the copy.
         curl_easy_setopt(easy, CURLOPT_POSTFIELDSIZE_LARGE, body_size) == CURLE_OK &&
         curl_easy_setopt(easy, CURLOPT_COPYPOSTFIELDS, body) == CURLE_OK &&
         curl_easy_setopt(easy, CURLOPT_HTTPHEADER, req->headers) == CURLE_OK &&
         curl_easy_setopt(easy, CURLOPT_WRITEFUNCTION, write_cb) == CURLE_OK &&
         curl_easy_setopt(easy, CURLOPT_WRITEDATA, req) == CURLE_OK &&
         curl_easy_setopt(easy, CURLOPT_ERRORBUFFER, req->errbuf) == CURLE_OK &&
         curl_easy_setopt(easy, CURLOPT_PRIVATE, req) == CURLE_OK;
}

HttpRequest *http_request(HttpClient *client, const HttpRequestOpts *opts, const HttpCallbacks *cb,
                          void *ud, Error *err)
{
  if (client->closing) {
    api_set_error(err, kErrorTypeValidation, "http: client is closing");
    return NULL;
  }
  if (opts->url == NULL) {
    api_set_error(err, kErrorTypeValidation, "http: url is required");
    return NULL;
  }
  if (opts->header_count > MUA_HTTP_MAX_HEADERS) {
    api_set_error(err, kErrorTypeValidation, "http: %zu headers exceeds cap of %d",
                  opts->header_count, MUA_HTTP_MAX_HEADERS);
    return NULL;
  }
  CURL *easy = curl_easy_init();
  if (easy == NULL) {
    api_set_error(err, kErrorTypeException, "http: curl_easy_init failed");
    return NULL;
  }
  HttpRequest *req = xcalloc(1, sizeof(*req));
  req->easy = easy;
  req->client = client;
  req->cb = *cb;
  req->ud = ud;
  for (size_t i = 0; i < opts->header_count; i++) { // bounded by MUA_HTTP_MAX_HEADERS
    struct curl_slist *appended = curl_slist_append(req->headers, opts->headers[i]);
    if (appended == NULL) {
      break; // OOM inside curl: the request will fail loudly server-side anyway
    }
    req->headers = appended;
  }
  if (!configure_transport(easy, opts) || !configure_request(req, opts)) {
    api_set_error(err, kErrorTypeException, "http: configuring the request failed");
    curl_slist_free_all(req->headers);
    curl_easy_cleanup(easy);
    xfree(req);
    return NULL;
  }
  CURLMcode mc = curl_multi_add_handle(client->multi, easy);
  if (mc != CURLM_OK) {
    api_set_error(err, kErrorTypeException, "http: %s", curl_multi_strerror(mc));
    curl_slist_free_all(req->headers);
    curl_easy_cleanup(easy);
    xfree(req);
    return NULL;
  }
  client->active++;
  req->next = client->requests;
  client->requests = req;
  return req;
}

void http_cancel(HttpRequest *req)
{
  if (req == NULL || req->cancel_requested) {
    return;
  }
  req->cancel_requested = true;
  // Defer the unwinding: this may be running inside a libcurl callback,
  // where curl_multi_remove_handle is illegal. A 0-ms timer is always a
  // legal context.
  if (uv_timer_start(&req->client->reap_timer, reap_cb, 0, 0) != 0) {
    log_msg(kLogError, "http: failed to arm the reap timer");
  }
}
