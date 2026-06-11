#ifndef MUA_LUA_STATE_H
#define MUA_LUA_STATE_H

#include <stdbool.h>

#include <lua.h>

// The Lua state singleton (documented global #2). Boots LuaJIT, prepends the
// runtime tree to package.path; the mua.api bridge registers here later.
bool mua_lua_init(void);
lua_State *mua_lua_state(void); // valid between mua_lua_init and teardown

// Evaluates <config>/init.lua if present. Errors in user config are NONFATAL
// (nvim semantics): the traceback goes to stderr and startup continues —
// returns false only on internal failure, never for a broken init.lua.
bool mua_lua_source_init(void);
void mua_lua_teardown(void); // idempotent

#endif // MUA_LUA_STATE_H
