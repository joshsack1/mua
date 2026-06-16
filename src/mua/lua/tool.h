#ifndef MUA_LUA_TOOL_H
#define MUA_LUA_TOOL_H

#include <stdbool.h>

#include "mua/api/private/defs.h"

// The core<->Lua seam for registered tools. tools.c stays Lua-agnostic and does
// the cJSON<->Object conversion itself; these (implemented in the bridge) speak
// Object only, so cJSON never crosses into the Lua layer. The analog of nvim
// core calling nlua_call_ref.

// Invokes the Lua function held at `callback` with `args` (borrowed; copied into
// Lua). Always produces a result: `*result_out` is an owned Object (free via
// api_free_object) -- a String for a string, empty (nil), or error result, or
// the marshaled value for a structured return. `*is_error_out` is true when the
// callback raised or its return could not be marshaled. Synchronous: the Lua
// VM is single-threaded, so a slow callback blocks the event loop.
void mua_lua_tool_invoke(LuaRef callback, Object args, Object *result_out, bool *is_error_out);

#endif // MUA_LUA_TOOL_H
