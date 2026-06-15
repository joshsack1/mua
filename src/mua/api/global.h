#ifndef MUA_API_GLOBAL_H
#define MUA_API_GLOBAL_H

#include "mua/api/private/defs.h"

// Global (unscoped) API functions -- the neovim-style public surface, exposed
// to Lua mechanically as mua.api.mua_* with identical names and argument order.
// These wrap the options store; sugar (mua.o) calls them, never the store.

// Sets agent option `name` to `value`. Unknown name or type/range mismatch
// sets a Validation error and leaves the store unchanged. Copies its inputs.
void mua_set_option(String name, Object value, Error *err) FUNC_API_SINCE(1);

// Returns the effective value of `name` (set value, else declared default) as
// an owned Object; string results are freed via api_free_string. Unknown name
// sets a Validation error and returns Nil.
Object mua_get_option(String name, Error *err) FUNC_API_SINCE(1);

#endif // MUA_API_GLOBAL_H
