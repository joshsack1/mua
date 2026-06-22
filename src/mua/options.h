#ifndef MUA_OPTIONS_H
#define MUA_OPTIONS_H

#include "mua/api/private/defs.h"

// The agent options store: documented mutable singleton #3 (after the event
// loop and the Lua state). Populated by init.lua before the agent runs -- via
// mua_set_option, the public API over this -- and read during request build.
// The `model` option may also change *between* turns: a UserPromptPre hook can
// reassign mua.o.model, so the REPL re-resolves and re-borrows the model at the
// start of each turn (after UserPromptPre) and never retains a borrow across a
// turn boundary. A pointer borrowed at a turn's start thus stays valid for that
// turn even though the store can change between turns. options_free releases
// every stored copy and resets the store to all-unset (also the unit-test
// isolation hook between cases).

// step_cap bounds, shared by the option table here and the agent's env clamp.
enum { MUA_STEP_CAP_MIN = 1, MUA_STEP_CAP_MAX = 50 };

// context_pct (hard stop) and context_warn_pct (soft warning) bounds: percent
// of the model's context window. The warn percent allows 0 to disable it.
enum { MUA_CONTEXT_PCT_MIN = 1, MUA_CONTEXT_PCT_MAX = 100 };
enum { MUA_CONTEXT_WARN_PCT_MIN = 0, MUA_CONTEXT_WARN_PCT_MAX = 100 };

// Sets option `name` to `value`. Validates that `name` is a known option and
// `value`'s type matches the option's declared type (integer options are also
// range-checked); on any mismatch sets a Validation error and leaves the store
// unchanged. Copies all referenced memory -- the caller keeps ownership of
// `value` and the bytes `name` views.
void options_set(String name, Object value, Error *err);

// Returns the effective value of `name` -- the set value, else the option's
// declared default -- as an owned Object; the caller frees a string result via
// api_free_string. An unknown `name` sets a Validation error and returns Nil.
Object options_get(String name, Error *err);

// Borrowing typed getters for the agent. The returned pointer/value is valid
// until the next options_set of that key or options_free.
const char *options_system_prompt(void); // effective prompt; "" means omit, never NULL
const char *options_model_borrow(void);  // set model, or NULL if unset (-> provider default)
int options_step_cap(void);              // effective cap, always within [MIN, MAX]
bool options_markdown(void);             // markdown rendering toggle; default false
int options_context_pct(void);           // context-window budget percent, within [MIN, MAX]
int options_context_warn_pct(void);      // soft-warning percent, within [MIN, MAX]; 0 disables

void options_free(void); // release all stored copies; reset to all-unset

#endif // MUA_OPTIONS_H
