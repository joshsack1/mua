#ifndef MUA_LUA_REF_H
#define MUA_LUA_REF_H

#include "mua/api/private/defs.h"

// Releases a Lua registry reference (luaL_unref). The shared seam by which
// Lua-agnostic core stores (the tool registry, the autocmd store) hand a
// LuaRef back to the bridge. Call before the Lua state is torn down; a no-op
// for an absent ref (LUA_NOREF/LUA_REFNIL).
void mua_lua_unref(LuaRef callback);

#endif // MUA_LUA_REF_H
