#ifndef MUA_AUTOCMD_H
#define MUA_AUTOCMD_H

#include <stddef.h>

#include "mua/api/private/defs.h"

// The autocmd store: Lua callbacks fired at fixed agent lifecycle events -- the
// nvim autocmd analog (documented mutable singleton #5). It holds LuaRefs
// (ints) only; the Lua bridge marshals payloads, invokes the callbacks, and
// releases the refs. Bounded like every accumulator (code-safety).

typedef enum {
  kAutocmdSessionStart = 0,
  kAutocmdSessionEnd,
  kAutocmdToolPre,
  kAutocmdToolPost,
  kAutocmdStreamDelta,
  kAutocmdUserPromptPre,
  kAutocmdEventCount, // sentinel: number of distinct events
} AutocmdEvent;

// Maps an event name ("ToolPre") to its enum, or -1 if unknown. `name` need not
// be NUL-terminated.
int autocmd_event_from_name(String name);

// Registers `callback` (a LuaRef the store now owns) for `event`. Returns the
// new autocmd's id (monotonic, >= 1). Sets a Validation error and returns 0
// (owning nothing -- the caller releases the ref) when the store is full.
Integer autocmd_create(AutocmdEvent event, LuaRef callback, Error *err);

// Number of callbacks registered for `event` -- the dispatch fast path.
size_t autocmd_count(AutocmdEvent event);

// The i-th callback registered for `event`; i must be < autocmd_count(event).
LuaRef autocmd_ref_at(AutocmdEvent event, size_t i);

// Releases every registered callback (via the Lua unref seam) and empties the
// store. clear is the Lua-callable reset; teardown is its alias, called before
// the Lua state is torn down -- the unref needs a live state.
void autocmd_clear(void);
void autocmd_teardown(void);

#endif // MUA_AUTOCMD_H
