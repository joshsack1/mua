#ifndef MUA_PROVIDER_OPENROUTER_H
#define MUA_PROVIDER_OPENROUTER_H

#include <stdint.h>

#include "mua/api/private/defs.h"
#include "mua/http.h"

// OpenRouter chat-completions streaming (OpenAI-compatible SSE). The provider
// owns retry policy: bounded attempts with full-jitter backoff, and only ever
// retries when zero user-visible output has been delivered.

// Verified against GET /api/v1/models (exact string; slash-namespaced).
#define MUA_DEFAULT_MODEL "anthropic/claude-sonnet-4.6"
#define MUA_OPENROUTER_BASE_URL "https://openrouter.ai/api/v1"
#define MUA_HTTP_MAX_ATTEMPTS 5

typedef struct OpenrouterStream OpenrouterStream;

typedef struct {
  const char *model;  // NULL selects MUA_DEFAULT_MODEL
  int64_t max_tokens; // <= 0: omitted from the request (per-model defaults)
  String prompt;
  const char *api_key;  // required; never logged, never quoted in Error messages
  const char *base_url; // NULL: $OPENROUTER_BASE_URL, then MUA_OPENROUTER_BASE_URL
} OpenrouterOpts;

// Exactly one of on_done/on_error fires, exactly once; the stream object is
// invalid once that terminal callback returns.
typedef struct {
  void (*on_text)(void *ud, const String *text);
  void (*on_done)(void *ud, const String *finish_reason, int64_t completion_tokens);
  void (*on_error)(void *ud, const Error *err);
} OpenrouterCallbacks;

OpenrouterStream *openrouter_stream(HttpClient *http, const OpenrouterOpts *opts,
                                    const OpenrouterCallbacks *cb, void *ud, Error *err);

// Cancels the in-flight attempt or pending retry; on_error("canceled") fires.
void openrouter_cancel(OpenrouterStream *stream);

#endif // MUA_PROVIDER_OPENROUTER_H
