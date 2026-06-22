#ifndef MUA_PROVIDER_MODELS_H
#define MUA_PROVIDER_MODELS_H

#include <stdint.h>

#include "mua/api/private/defs.h"
#include "mua/http.h"

// Model-catalog metadata + cache. OpenRouter's usage object reports tokens used
// but not the model's context window; that lives in GET <base>/models. This
// module fetches the catalog ONCE -- the first time a window is needed -- parses
// every model's window into a process-level cache (documented mutable singleton
// #8, models.c), and serves all later lookups locally, so switching models
// mid-session costs no extra network. The catalog is immutable for a session.

// Returns model_id's context window in tokens (NULL/empty selects the built-in
// default). Synchronous: on a cache miss it spins the event loop to fetch the
// whole catalog, caches it, then looks up; subsequent calls (any model) are
// local scans. Returns 0 -- never an Error -- on any failure (HTTP error, model
// absent from the catalog, field missing); the caller treats 0 as "budget
// disabled, the step cap still applies". A failed fetch is not cached, so a
// later call retries.
int64_t models_context_length(HttpClient *http, const char *model_id, const char *api_key);

// Releases the cache and resets it to empty/unfetched. Call once at process
// shutdown; also the unit-test isolation hook between cases.
void models_cache_free(void);

// Test seams (no network). models_cache_populate parses a catalog JSON body into
// the cache and marks it fetched (parseable or not); models_cache_window reads a
// cached window (0 when the model is absent or its window is unadvertised).
void models_cache_populate(String body);
int64_t models_cache_window(const char *model_id);

#endif // MUA_PROVIDER_MODELS_H
