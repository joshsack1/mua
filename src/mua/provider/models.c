#include "mua/provider/models.h"

#include <stdlib.h>
#include <string.h>

#include <cjson/cJSON.h>
#include <curl/curl.h>

#include "mua/api/private/helpers.h"
#include "mua/json.h"
#include "mua/log.h"
#include "mua/loop.h"
#include "mua/memory.h"
#include "mua/provider/openrouter.h" // MUA_DEFAULT_MODEL, MUA_OPENROUTER_BASE_URL

enum {
  kModelsBodyCap = 8 * 1024 * 1024, // the catalog is a few MB; bound it generously
  kModelsMaxLoopTurns = 100000,     // backstop; real termination is curl's timeouts
  kModelsCacheMax = 4096,           // hard cap on cached entries (the body cap bounds input too)
  kModelsCloseDrainMax = 64,        // bounded passes to drain the dedicated client's handle closes
};

// Documented mutable singleton #8: the model-catalog cache. Populated once from a
// single GET <base>/models; thereafter every window lookup is a local scan.
// Bounded by kModelsCacheMax; the catalog is immutable for a session, so this is
// not mutated once fetched. models_cache_free resets it (process shutdown +
// unit-test isolation between cases).
typedef struct {
  char *id;       // owned model id
  int64_t window; // context_length in tokens (0 = absent/unadvertised)
} CatalogEntry;
static CatalogEntry *g_catalog;
static size_t g_catalog_len;
static bool g_catalog_fetched; // a full catalog fetch has succeeded

// Collected response for one catalog fetch; body is borrowed from the caller.
typedef struct {
  Buf *body;
  long status;
  bool done;
  bool ok;
} ModelsFetch;

static void models_on_status(void *ud, long status, const char *content_type)
{
  (void)content_type;
  ((ModelsFetch *)ud)->status = status;
}

static bool models_on_chunk(void *ud, const char *bytes, size_t len)
{
  ModelsFetch *fetch = ud;
  return buf_append(fetch->body, bytes, len); // false past the cap aborts the transfer
}

static void models_on_complete(void *ud, CURLcode result, long http_status, const char *errmsg,
                               long retry_after_s)
{
  (void)errmsg;
  (void)retry_after_s;
  ModelsFetch *fetch = ud;
  long status = (http_status != 0) ? http_status : fetch->status;
  fetch->ok = (result == CURLE_OK && status == 200);
  fetch->done = true;
}

// A model entry's context window: the serving provider's value, else the
// top-level field, else 0 when either is absent or non-positive.
static int64_t entry_window(const cJSON *m)
{
  int64_t top_ctx = 0;
  int64_t own_ctx = 0;
  cJSON *top = json_get_obj(m, "top_provider");
  (void)(top != NULL && json_get_int(top, "context_length", &top_ctx));
  (void)json_get_int(m, "context_length", &own_ctx);
  return (top_ctx > 0) ? top_ctx : (own_ctx > 0 ? own_ctx : 0);
}

static int find_entry(const char *id)
{
  for (size_t i = 0; i < g_catalog_len; i++) { // bounded by kModelsCacheMax
    if (strcmp(g_catalog[i].id, id) == 0) {
      return (int)i;
    }
  }
  return -1;
}

void models_cache_free(void)
{
  for (size_t i = 0; i < g_catalog_len; i++) {
    xfree(g_catalog[i].id);
  }
  xfree(g_catalog);
  g_catalog = NULL;
  g_catalog_len = 0;
  g_catalog_fetched = false;
}

void models_cache_populate(String body)
{
  models_cache_free();      // idempotent: drop any prior catalog before repopulating
  g_catalog_fetched = true; // a fetch produced this body, parseable or not
  Error err = ERROR_INIT;
  cJSON *doc = json_parse(body, kModelsBodyCap, &err);
  api_clear_error(&err); // an unparseable catalog is not itself an error here
  if (doc == NULL) {
    return;
  }
  cJSON *data = json_get_arr(doc, "data");
  if (data != NULL) {
    size_t cap = 0;
    const cJSON *m = data->child;
    for (; m != NULL && g_catalog_len < kModelsCacheMax; m = m->next) { // bounded
      const char *id = json_get_cstr(m, "id");
      if (id == NULL) {
        continue;
      }
      if (g_catalog_len == cap) {
        cap = (cap == 0) ? 64 : cap * 2;
        if (cap > kModelsCacheMax) {
          cap = kModelsCacheMax;
        }
        g_catalog = xrealloc(g_catalog, cap * sizeof(*g_catalog));
      }
      g_catalog[g_catalog_len].id = xstrdup(id);
      g_catalog[g_catalog_len].window = entry_window(m);
      g_catalog_len++;
    }
    if (m != NULL) { // stopped at the cap with entries remaining
      log_msg(kLogWarn, "models: catalog exceeds %d entries; extras not cached", kModelsCacheMax);
    }
  }
  json_free(doc);
}

int64_t models_cache_window(const char *model_id)
{
  if (model_id == NULL) {
    return 0;
  }
  int idx = find_entry(model_id);
  return idx >= 0 ? g_catalog[idx].window : 0;
}

// Builds "<base>/models" with the same base resolution as the provider.
static char *build_models_url(void)
{
  const char *base = getenv("OPENROUTER_BASE_URL");
  if (base == NULL || base[0] == '\0') {
    base = MUA_OPENROUTER_BASE_URL;
  }
  size_t base_len = strlen(base);
  while (base_len > 0 && base[base_len - 1] == '/') {
    base_len--;
  }
  static const char path[] = "/models";
  char *url = xmalloc(base_len + sizeof(path));
  memcpy(url, base, base_len);
  memcpy(url + base_len, path, sizeof(path)); // includes the NUL
  return url;
}

// One synchronous GET <base>/models, appending the response into *out. Returns
// whether a 200 catalog body arrived (errors are logged, never raised).
//
// The fetch runs on a DEDICATED, short-lived HttpClient -- never the agent's
// long-lived streaming client. Its request userdata is the stack `fetch` below;
// a request left registered on the shared client could fire its write callback
// after this frame returns (HTTP/2 connection reuse plus curl's deferred
// client-write buffering can defer body delivery to a later loop run),
// dereferencing the dead-stack `fetch->body` and crashing in buf_append.
// http_client_close finishes every request synchronously (removing it from
// curl), so once it returns no catalog request survives into the agent's turns;
// the bounded non-blocking drain then runs the deferred handle-close callbacks.
static bool fetch_catalog_body(HttpClient *agent_http, const char *api_key, Buf *out)
{
  if (agent_http == NULL || api_key == NULL || api_key[0] == '\0') {
    return false;
  }
  Error cerr = ERROR_INIT;
  HttpClient *client = http_client_new(http_client_loop(agent_http), &cerr);
  if (client == NULL) {
    log_msg(kLogWarn, "models: catalog client init failed: %s", cerr.msg != NULL ? cerr.msg : "?");
    api_clear_error(&cerr);
    return false;
  }
  ModelsFetch fetch = {.body = out};
  char *url = build_models_url();
  static const char bearer_prefix[] = "Authorization: Bearer ";
  size_t key_len = strlen(api_key);
  char *auth = xmalloc(sizeof(bearer_prefix) + key_len);
  memcpy(auth, bearer_prefix, sizeof(bearer_prefix) - 1);
  memcpy(auth + sizeof(bearer_prefix) - 1, api_key, key_len + 1);
  const char *headers[] = {auth, "Accept: application/json"};
  HttpRequestOpts opts = {
    .url = url,
    .headers = headers,
    .header_count = sizeof(headers) / sizeof(headers[0]),
    .get = true,
  };
  HttpCallbacks cb = {
    .on_status = models_on_status,
    .on_chunk = models_on_chunk,
    .on_complete = models_on_complete,
  };
  Error err = ERROR_INIT;
  HttpRequest *req = http_request(client, &opts, &cb, &fetch, &err);
  if (req == NULL) {
    log_msg(kLogWarn, "models: catalog request failed: %s", err.msg != NULL ? err.msg : "?");
    api_clear_error(&err);
  } else {
    for (int i = 0; i < kModelsMaxLoopTurns && !fetch.done; i++) {
      if (loop_run_once() == 0 && !fetch.done) {
        break; // no handles left but the request never finished
      }
    }
    if (!fetch.done) {
      log_msg(kLogWarn, "models: catalog fetch did not complete");
    } else if (!fetch.ok) {
      log_msg(kLogWarn, "models: catalog fetch failed (HTTP %ld)", fetch.status);
    }
  }
  // Tear down the dedicated client before `fetch` and `auth` leave scope.
  // http_client_close finishes any still-registered request synchronously, so no
  // catalog write callback can fire afterwards; the bounded drain then runs the
  // deferred handle-close callbacks. loop_run_nowait never blocks, so this cannot
  // hang on the agent client's idle handles, which share this loop.
  http_client_close(client);
  for (int i = 0; i < kModelsCloseDrainMax; i++) {
    if (loop_run_nowait() == 0) {
      break; // no active handles remain: the closes are done
    }
  }
  // The bearer key lived in `auth`; do not leave it in freed heap pages.
  memset(auth, 0, strlen(auth));
  xfree(auth);
  xfree(url);
  return fetch.ok;
}

int64_t models_context_length(HttpClient *http, const char *model_id, const char *api_key)
{
  const char *id = (model_id != NULL && model_id[0] != '\0') ? model_id : MUA_DEFAULT_MODEL;
  int idx = find_entry(id);
  if (idx >= 0) {
    return g_catalog[idx].window; // cache hit: no network
  }
  if (g_catalog_fetched) {
    return 0; // catalog already fetched; this model simply isn't in it
  }
  Buf body;
  buf_init(&body, kModelsBodyCap);
  bool ok = fetch_catalog_body(http, api_key, &body);
  if (ok) {
    models_cache_populate((String){.data = body.data, .size = body.size});
  }
  buf_free(&body);
  idx = find_entry(id);
  if (idx >= 0) {
    return g_catalog[idx].window;
  }
  if (ok) { // we have the catalog; the model genuinely isn't in it
    log_msg(kLogWarn, "models: context_length unknown for '%s'", id);
  }
  return 0;
}
