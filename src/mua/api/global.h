#ifndef MUA_API_GLOBAL_H
#define MUA_API_GLOBAL_H

#include "mua/api/private/defs.h"

// Global (unscoped) API functions -- the neovim-style public surface, exposed
// to Lua mechanically as mua.api.mua_* with identical names and argument order.
// These wrap the C-side stores; the sugar (mua.o, mua.g) calls them, never the
// stores directly.

// Sets agent option `name` to `value`. Unknown name or type/range mismatch
// sets a Validation error and leaves the store unchanged. Copies its inputs.
void mua_set_option(String name, Object value, Error *err) FUNC_API_SINCE(1);

// Returns the effective value of `name` (set value, else declared default) as
// an owned Object; string results are freed via api_free_string. Unknown name
// sets a Validation error and returns Nil.
Object mua_get_option(String name, Error *err) FUNC_API_SINCE(1);

// Sets global variable `name` to `value` (a Nil `value` deletes it); copies its
// inputs. Backs mua.g (the vim.g analog), arbitrary keys, any value.
void mua_set_var(String name, Object value, Error *err) FUNC_API_SINCE(2);

// Returns global variable `name` as an owned Object (free via api_free_object);
// an unset name returns Nil, not an error.
Object mua_get_var(String name, Error *err) FUNC_API_SINCE(2);

// Registers a model-callable tool. `schema` is its JSON Schema parameters (a
// Dict; Nil -> an empty-object schema); `mutating` routes it through the
// approval gate; `callback` is a LuaRef the registry owns and invokes. Sets a
// Validation error (registering nothing) on an empty or duplicate name, a
// non-Dict schema, or a full registry. `name`/`description` are copied.
void mua_register_tool(String name, String description, Object schema, Boolean mutating,
                       LuaRef callback, Error *err) FUNC_API_SINCE(3);

// Registers a Lua callback (a LuaRef the store now owns) for lifecycle event
// `event` -- one of "SessionStart"/"SessionEnd"/"ToolPre"/"ToolPost"/
// "StreamDelta". Returns the new autocmd's id; an unknown event or a full store
// sets a Validation error and returns 0. The nvim_create_autocmd analog.
Integer mua_create_autocmd(String event, LuaRef callback, Error *err) FUNC_API_SINCE(3);

// Removes every registered autocmd, releasing their callbacks.
void mua_clear_autocmds(void);

#endif // MUA_API_GLOBAL_H
