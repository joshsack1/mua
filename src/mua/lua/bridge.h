#ifndef MUA_LUA_BRIDGE_H
#define MUA_LUA_BRIDGE_H

#include <stdbool.h>

#include <lua.h>

// Builds the global `mua` table with its `mua.api` C function surface, then
// loads the shipped Lua sugar (mua.o) on top. Call once, after package.path is
// set and before user init.lua is sourced. Returns false (and reports on
// stderr) only if the shipped runtime fails to load -- a broken install, not
// user error.
bool mua_lua_bridge_init(lua_State *lstate);

#endif // MUA_LUA_BRIDGE_H
