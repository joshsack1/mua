#ifndef MUA_PROVIDER_MODELS_H
#define MUA_PROVIDER_MODELS_H

#include <stdint.h>

#include "mua/api/private/defs.h"
#include "mua/http.h"

// Model-catalog metadata. OpenRouter's usage object reports tokens used but not
// the model's context window; that lives in GET /api/v1/models. This module
// fetches the window once at startup so the agent can budget against it.

// Fetches the catalog (GET <base>/models), finds `model_id` (NULL/empty selects
// the built-in default), and returns its context_length in tokens. Synchronous
// from the caller's view: spins the event loop until the request completes.
// Returns 0 -- never an Error -- on any failure (HTTP error, model absent,
// field missing, oversize body); the caller treats 0 as "budget disabled, step
// cap still applies" and a warning is logged. base_url mirrors the provider's
// resolution (NULL -> $OPENROUTER_BASE_URL -> the built-in default).
int64_t models_fetch_context_length(HttpClient *http, const char *model_id, const char *api_key,
                                     const char *base_url);

// The pure parse step (test seam): finds `model_id` in a catalog JSON body and
// returns its context_length, preferring top_provider.context_length over the
// top-level field. 0 when absent or unparseable. `model_id` must be non-NULL.
int64_t models_parse_context_length(String body, const char *model_id);

#endif // MUA_PROVIDER_MODELS_H
