#include "mua/lua/bridge.h"

#include <stdio.h>

#include <lauxlib.h>

#include "mua/api/global.h"
#include "mua/api/private/helpers.h"

// The C<->Lua bridge: it exposes the public C API mechanically as mua.api.mua_*
// (identical names and argument order) and layers the mua.o sugar on top. All
// option storage lives in C; this layer only marshals values across and
// translates API errors into Lua errors.

// Doubles represent every integer exactly up to 2^53; beyond that, treat a Lua
// number as a float rather than risk an out-of-range double->int64 conversion.
static const double kExactIntMax = 9007199254740992.0; // 2^53

// Marshal a Lua value (string / number / boolean / nil only) to an Object. The
// string case borrows Lua's buffer -- options_set copies it before the value
// leaves the stack. Length-prefixed: String is not NUL-terminated and a value
// may carry embedded NULs.
static Object lua_to_object(lua_State *lstate, int idx)
{
  switch (lua_type(lstate, idx)) {
  case LUA_TSTRING: {
    size_t len = 0;
    const char *str = lua_tolstring(lstate, idx, &len);
    return (Object){.type = kObjectTypeString, .data.string = {.data = (char *)str, .size = len}};
  }
  case LUA_TNUMBER: {
    double num = (double)lua_tonumber(lstate, idx);
    if (num >= -kExactIntMax && num <= kExactIntMax && num == (double)(Integer)num) {
      return (Object){.type = kObjectTypeInteger, .data.integer = (Integer)num};
    }
    return (Object){.type = kObjectTypeFloat, .data.floating = num};
  }
  case LUA_TBOOLEAN:
    return (Object){.type = kObjectTypeBoolean, .data.boolean = lua_toboolean(lstate, idx) != 0};
  default:
    return (Object){.type = kObjectTypeNil};
  }
}

// Raise a Lua error carrying `err`'s message, freeing the C error first:
// lua_error longjmps and never returns, so a clear placed after it would leak.
static int raise_api_error(lua_State *lstate, Error *err)
{
  lua_pushstring(lstate, err->msg != NULL ? err->msg : "option error");
  api_clear_error(err);
  return lua_error(lstate);
}

static int l_mua_set_option(lua_State *lstate)
{
  size_t name_len = 0;
  const char *name = luaL_checklstring(lstate, 1, &name_len);
  int value_type = lua_type(lstate, 2);
  if (value_type != LUA_TSTRING && value_type != LUA_TNUMBER && value_type != LUA_TBOOLEAN &&
      value_type != LUA_TNIL) {
    return luaL_argerror(lstate, 2, "option value must be a string, number, boolean, or nil");
  }
  String name_s = {.data = (char *)name, .size = name_len};
  Object value = lua_to_object(lstate, 2);
  Error err = ERROR_INIT;
  mua_set_option(name_s, value, &err);
  if (ERROR_SET(&err)) {
    return raise_api_error(lstate, &err);
  }
  return 0;
}

static int l_mua_get_option(lua_State *lstate)
{
  size_t name_len = 0;
  const char *name = luaL_checklstring(lstate, 1, &name_len);
  String name_s = {.data = (char *)name, .size = name_len};
  Error err = ERROR_INIT;
  Object value = mua_get_option(name_s, &err);
  if (ERROR_SET(&err)) {
    return raise_api_error(lstate, &err);
  }
  if (value.type == kObjectTypeString) {
    lua_pushlstring(lstate, value.data.string.data != NULL ? value.data.string.data : "",
                    value.data.string.size);
    api_free_string(value.data.string); // free the owned copy once Lua has copied it
    return 1;
  }
  switch (value.type) {
  case kObjectTypeNil:
    lua_pushnil(lstate);
    break;
  case kObjectTypeBoolean:
    lua_pushboolean(lstate, value.data.boolean);
    break;
  case kObjectTypeInteger:
    lua_pushinteger(lstate, (lua_Integer)value.data.integer);
    break;
  case kObjectTypeFloat:
    lua_pushnumber(lstate, value.data.floating);
    break;
  default:
    return luaL_error(lstate, "option '%s' has an unsupported value type", name);
  }
  return 1;
}

// Registered mechanically; mua.api.mua_* mirrors the C names one-to-one.
static const luaL_Reg api_functions[] = {
    {"mua_set_option", l_mua_set_option},
    {"mua_get_option", l_mua_get_option},
    {NULL, NULL},
};

bool mua_lua_bridge_init(lua_State *lstate)
{
  lua_newtable(lstate); // the `mua` table
  lua_newtable(lstate); // the `mua.api` subtable
  for (const luaL_Reg *fn = api_functions; fn->name != NULL; fn++) {
    lua_pushcfunction(lstate, fn->func);
    lua_setfield(lstate, -2, fn->name);
  }
  lua_setfield(lstate, -2, "api"); // mua.api = { ... }
  lua_setglobal(lstate, "mua");    // global `mua`, nvim-style (like `vim`)

  // Layer the shipped sugar (mua.o) onto the global table. require runs
  // runtime/lua/mua/init.lua via package.path and caches the result, so a
  // later require("mua") -- from user init.lua -- returns the same table.
  if (luaL_dostring(lstate, "require('mua')") != 0) {
    const char *msg = lua_tostring(lstate, -1);
    (void)fprintf(stderr, "mua: failed to load mua runtime: %s\n", msg != NULL ? msg : "?");
    lua_pop(lstate, 1);
    return false;
  }
  return true;
}
