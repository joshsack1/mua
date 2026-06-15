#ifndef MUA_VARIABLES_H
#define MUA_VARIABLES_H

#include "mua/api/private/defs.h"

// The global-variables store behind mua.g (the vim.g analog): documented
// mutable singleton #4 (after the event loop, the Lua state, and the options
// store). Open-ended -- arbitrary string keys, any Object value -- unlike the
// fixed-schema options store. Populated from init.lua via mua_set_var and read
// back via mua_get_var; not read by the agent itself this slice. variables_free
// releases every stored key and value (also the unit-test isolation hook).

// Maximum number of distinct variables. Bounds the store (code-safety: every
// accumulator is bounded); a new key past this is a Validation error, never a
// silent grow.
enum { MUA_VAR_MAX = 1024 };

// Sets variable `name` to `value`. A Nil `value` deletes `name` (a no-op when
// unset), mirroring `vim.g.x = nil`. Otherwise stores a deep copy, replacing any
// prior value; a new key past MUA_VAR_MAX sets a Validation error and leaves the
// store unchanged. Copies all referenced memory -- the caller keeps ownership of
// `value` and the bytes `name` views.
void variables_set(String name, Object value, Error *err);

// Returns the value of `name` as an owned Object (free strings/containers via
// api_free_object). An unset `name` returns Nil and leaves `err` untouched --
// reading an undefined global is not an error (the deliberate difference from
// options_get).
Object variables_get(String name, Error *err);

void variables_free(void); // release all stored keys and values; reset to empty

#endif // MUA_VARIABLES_H
