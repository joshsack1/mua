#ifndef MUA_OPTIONS_H
#define MUA_OPTIONS_H

#include "mua/api/private/defs.h"

// The agent options store: documented mutable singleton #3 (after the event
// loop and the Lua state). Populated by init.lua before the agent runs -- via
// mua_set_option, the public API over this -- and read during request build.
// It is never mutated mid-run, so a pointer a turn borrows at its start stays
// valid for the turn. options_free releases every stored copy and resets the
// store to all-unset (also the unit-test isolation hook between cases).

// step_cap bounds, shared by the option table here and the agent's env clamp.
enum { MUA_STEP_CAP_MIN = 1, MUA_STEP_CAP_MAX = 50 };

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

void options_free(void); // release all stored copies; reset to all-unset

#endif // MUA_OPTIONS_H
