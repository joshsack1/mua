#ifndef MUA_PROVIDER_OPENROUTER_INTERNAL_H
#define MUA_PROVIDER_OPENROUTER_INTERNAL_H

#include <stdbool.h>

#include "mua/provider/openrouter.h"

// Test seams (busted + FFI drive these without any network or event loop).

// The real per-event dispatch: [DONE] sentinel, error-in-data, delta text,
// finish_reason/usage latches. Returns false when the transfer should stop.
bool openrouter_handle_event(OpenrouterStream *stream, String data);

// A bare stream carrying only callbacks and dispatch state — no HTTP request,
// no retry timer. Free with openrouter_stream_free_for_test only.
OpenrouterStream *openrouter_stream_new_for_test(const OpenrouterCallbacks *cb, void *ud);
void openrouter_stream_free_for_test(OpenrouterStream *stream);

#endif // MUA_PROVIDER_OPENROUTER_INTERNAL_H
