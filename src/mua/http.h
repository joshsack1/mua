#ifndef MUA_HTTP_H
#define MUA_HTTP_H

#include <stdbool.h>
#include <stddef.h>

#include <curl/curl.h>
#include <uv.h>

#include "mua/api/private/defs.h"

// curl multi_socket engine bridged to libuv. Single-threaded by design:
// write callbacks, completion handling, and user callbacks all run on the
// loop thread; there are no locks anywhere on the data path. One attempt per
// request — retry policy lives in the provider, which alone knows whether
// user-visible output was already delivered.

#define MUA_HTTP_MAX_HEADERS 32
#define MUA_HTTP_CONNECT_TIMEOUT_MS 10000L
#define MUA_HTTP_STALL_WINDOW_S 90L

typedef struct HttpClient HttpClient;
typedef struct HttpRequest HttpRequest;

typedef struct {
  const char *url;
  const char *const *headers; // "Name: value" strings, copied
  size_t header_count;        // <= MUA_HTTP_MAX_HEADERS
  String body;                // POST body, copied (ignored when `get` is set)
  bool get;                   // true: issue a GET (no body); default is POST
  long connect_timeout_ms;    // 0 selects MUA_HTTP_CONNECT_TIMEOUT_MS
  long stall_window_s;        // 0 selects MUA_HTTP_STALL_WINDOW_S
} HttpRequestOpts;

typedef struct {
  // Fired exactly once, lazily at the first body bytes (curl guarantees
  // headers are fully processed by then), or from completion for bodyless
  // responses. content_type may be NULL.
  void (*on_status)(void *ud, long status, const char *content_type);
  // Arbitrary-size chunk of the (already content-decoded) body. Return false
  // to abort the transfer; CURLE_WRITE_ERROR surfaces in on_complete.
  bool (*on_chunk)(void *ud, const char *bytes, size_t len);
  // Fired exactly once per submitted request, last — including after
  // http_cancel (CURLE_ABORTED_BY_CALLBACK). The request handle and its
  // userdata are dead once this returns. retry_after_s is -1 if absent.
  void (*on_complete)(void *ud, CURLcode result, long http_status, const char *errmsg,
                      long retry_after_s);
} HttpCallbacks;

// One client per loop (the process's documented instance lives in main).
// Performs curl_global_init on first use.
HttpClient *http_client_new(uv_loop_t *loop, Error *err);

uv_loop_t *http_client_loop(const HttpClient *client);

// Cancels every in-flight request (their on_complete callbacks fire), then
// tears down asynchronously: run the loop afterwards to drain handle closes.
void http_client_close(HttpClient *client);

HttpRequest *http_request(HttpClient *client, const HttpRequestOpts *opts, const HttpCallbacks *cb,
                          void *ud, Error *err);

// Idempotent; callable from any callback context (the actual unwinding is
// deferred to a 0-ms timer because curl_multi_remove_handle is illegal
// inside libcurl callbacks).
void http_cancel(HttpRequest *req);

// Pairs with the lazy curl_global_init; call once at process exit.
void http_global_cleanup(void);

#endif // MUA_HTTP_H
