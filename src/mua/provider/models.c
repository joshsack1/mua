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
};

// Collected response for one catalog fetch.
typedef struct {
  Buf body;
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
  return buf_append(&fetch->body, bytes, len); // false past the cap aborts the transfer
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

int64_t models_parse_context_length(String body, const char *model_id)
{
  if (model_id == NULL || model_id[0] == '\0') {
    return 0;
  }
  Error err = ERROR_INIT;
  cJSON *doc = json_parse(body, kModelsBodyCap, &err);
  api_clear_error(&err); // an unparseable catalog is not itself an error here
  if (doc == NULL) {
    return 0;
  }
  int64_t result = 0;
  cJSON *data = json_get_arr(doc, "data");
  if (data != NULL) {
    for (const cJSON *m = data->child; m != NULL; m = m->next) { // bounded sibling walk
      const char *id = json_get_cstr(m, "id");
      if (id == NULL || strcmp(id, model_id) != 0) {
        continue;
      }
      // Prefer the serving provider's window over the top-level field; fall
      // back when either is absent or non-positive.
      int64_t top_ctx = 0;
      int64_t own_ctx = 0;
      cJSON *top = json_get_obj(m, "top_provider");
      (void)(top != NULL && json_get_int(top, "context_length", &top_ctx));
      (void)json_get_int(m, "context_length", &own_ctx);
      result = (top_ctx > 0) ? top_ctx : (own_ctx > 0 ? own_ctx : 0);
      break;
    }
  }
  json_free(doc);
  return result;
}

// Builds "<base>/models" with the same base resolution as the provider.
static char *build_models_url(const char *base_url)
{
  const char *base = base_url;
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
  static const char path[] = "/models";
  char *url = xmalloc(base_len + sizeof(path));
  memcpy(url, base, base_len);
  memcpy(url + base_len, path, sizeof(path)); // includes the NUL
  return url;
}

int64_t models_fetch_context_length(HttpClient *http, const char *model_id, const char *api_key,
                                     const char *base_url)
{
  if (http == NULL || api_key == NULL || api_key[0] == '\0') {
    return 0;
  }
  const char *id = (model_id != NULL && model_id[0] != '\0') ? model_id : MUA_DEFAULT_MODEL;
  ModelsFetch fetch = {0};
  buf_init(&fetch.body, kModelsBodyCap);
  char *url = build_models_url(base_url);
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
  int64_t result = 0;
  Error err = ERROR_INIT;
  HttpRequest *req = http_request(http, &opts, &cb, &fetch, &err);
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
    } else {
      result =
        models_parse_context_length((String){.data = fetch.body.data, .size = fetch.body.size}, id);
      if (result == 0) {
        log_msg(kLogWarn, "models: context_length unknown for '%s'", id);
      }
    }
  }
  // The bearer key lived in `auth`; do not leave it in freed heap pages.
  memset(auth, 0, strlen(auth));
  xfree(auth);
  xfree(url);
  buf_free(&fetch.body);
  return result;
}
