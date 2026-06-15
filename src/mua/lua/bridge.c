#include "mua/lua/bridge.h"

#include <stdio.h>
#include <string.h>

#include <lauxlib.h>

#include "mua/api/global.h"
#include "mua/api/private/helpers.h"
#include "mua/memory.h"

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
  lua_pushstring(lstate, err->msg != NULL ? err->msg : "api error");
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

// --- Value marshaling: arbitrary Lua values <-> Object (nested Array/Dict) ----
// Both directions walk an explicit, depth-capped frame stack (kMarshalDepthCap)
// rather than recursing, per json.h's tree-walk rule; the cap is also the cycle
// guard on the Lua->Object side. `String` is never assumed NUL-terminated:
// lengths flow through lua_tolstring/lua_pushlstring throughout.

// Converts the Lua scalar at absolute index `idx` to an Object. String bytes are
// copied into `arena` (transient; the store deep-copies onto the heap later).
static Object scalarize(lua_State *lstate, int idx, Arena *arena)
{
  switch (lua_type(lstate, idx)) {
    case LUA_TBOOLEAN:
      return BOOLEAN_OBJ(lua_toboolean(lstate, idx) != 0);
    case LUA_TNUMBER: {
      double num = (double)lua_tonumber(lstate, idx);
      if (num >= -kExactIntMax && num <= kExactIntMax && num == (double)(Integer)num) {
        return INTEGER_OBJ((Integer)num);
      }
      return FLOAT_OBJ(num);
    }
    case LUA_TSTRING: {
      size_t len = 0;
      const char *str = lua_tolstring(lstate, idx, &len);
      char *buf = arena_alloc(arena, len);
      if (len > 0) {
        memcpy(buf, str, len);
      }
      return (Object){.type = kObjectTypeString, .data.string = {.data = buf, .size = len}};
    }
    default:
      return NIL; // LUA_TNIL, or a type the caller already rejected
  }
}

static void push_scalar(lua_State *lstate, const Object *obj)
{
  switch (obj->type) {
    case kObjectTypeBoolean:
      lua_pushboolean(lstate, obj->data.boolean);
      break;
    case kObjectTypeInteger:
      lua_pushinteger(lstate, (lua_Integer)obj->data.integer);
      break;
    case kObjectTypeFloat:
      lua_pushnumber(lstate, obj->data.floating);
      break;
    case kObjectTypeString:
      lua_pushlstring(lstate, obj->data.string.data != NULL ? obj->data.string.data : "",
                      obj->data.string.size);
      break;
    default:
      lua_pushnil(lstate); // Nil / unexpected
      break;
  }
}

typedef enum { kTableArray, kTableDict, kTableInvalid } TableKind;

// Classifies the table at absolute index `idx` and counts its entries. Array iff
// its keys are exactly the integers 1..cnt; Dict iff every key is a string;
// empty -> Array. Mixed, holed, or non-string/non-posint keys -> Invalid. Reads
// number keys with lua_tonumber (not lua_tolstring, which would mutate a key
// mid-iteration and confuse lua_next).
static TableKind classify_table(lua_State *lstate, int idx, size_t *cnt_out)
{
  size_t cnt = 0;
  bool all_string = true;
  bool all_posint = true;
  size_t max_int = 0;
  lua_pushnil(lstate);
  while (lua_next(lstate, idx) != 0) {
    cnt++;
    int kt = lua_type(lstate, -2);
    if (kt == LUA_TSTRING) {
      all_posint = false;
    } else if (kt == LUA_TNUMBER) {
      all_string = false;
      double k = (double)lua_tonumber(lstate, -2);
      lua_Integer ki = (lua_Integer)k;
      if (k == (double)ki && ki >= 1) {
        if ((size_t)ki > max_int) {
          max_int = (size_t)ki;
        }
      } else {
        all_posint = false;
      }
    } else {
      lua_pop(lstate, 2); // pop value + key, abandon iteration
      *cnt_out = cnt;
      return kTableInvalid;
    }
    lua_pop(lstate, 1); // pop value, keep key for the next lua_next
  }
  *cnt_out = cnt;
  if (cnt == 0) {
    return kTableArray; // empty table -> Array (documented choice)
  }
  if (all_string) {
    return kTableDict;
  }
  if (all_posint && max_int == cnt) {
    return kTableArray;
  }
  return kTableInvalid;
}

// Classifies the table at `idx`, allocates `out`'s backing array in `arena`, and
// for a Dict gathers the (string) keys into out->items[].key. Element values are
// left Nil for the build walk. Sets `err` + returns false on an unclassifiable
// table. `idx` must be a positive (absolute) stack index.
static bool table_init(lua_State *lstate, int idx, Arena *arena, Object *out, Error *err)
{
  size_t cnt = 0;
  TableKind kind = classify_table(lstate, idx, &cnt);
  if (kind == kTableInvalid) {
    api_set_error(err, kErrorTypeValidation,
                  "cannot store a table with mixed, holed, or non-string keys");
    return false;
  }
  if (kind == kTableArray) {
    out->type = kObjectTypeArray;
    out->data.array.size = cnt;
    out->data.array.capacity = cnt;
    out->data.array.items = (cnt > 0) ? arena_alloc(arena, cnt * sizeof(Object)) : NULL;
    for (size_t i = 0; i < cnt; i++) {
      out->data.array.items[i] = NIL; // well-formed if the build errors mid-fill
    }
    return true;
  }
  out->type = kObjectTypeDict;
  out->data.dict.size = cnt;
  out->data.dict.capacity = cnt;
  out->data.dict.items = (cnt > 0) ? arena_alloc(arena, cnt * sizeof(KeyValuePair)) : NULL;
  size_t i = 0;
  lua_pushnil(lstate);
  while (lua_next(lstate, idx) != 0) {
    // Keys are all strings here (classify_table verified it), so lua_tolstring
    // does not coerce the key in place -- safe to call mid-iteration.
    size_t klen = 0;
    const char *kdata = lua_tolstring(lstate, -2, &klen);
    char *buf = arena_alloc(arena, klen);
    if (klen > 0) {
      memcpy(buf, kdata, klen);
    }
    out->data.dict.items[i].key = (String){.data = buf, .size = klen};
    out->data.dict.items[i].value = NIL;
    i++;
    lua_pop(lstate, 1); // pop value, keep key
  }
  return true;
}

typedef struct {
  Object *container; // the Array/Dict being filled (arena-backed, or the root out)
  size_t n;          // child count
  size_t next;       // next child to fetch
  int lua_idx;       // absolute index of the source Lua table
  bool is_array;
  bool pop_table; // pop the source table on completion (false for the root arg)
} PopFrame;

// Builds an owned-by-`arena` Object tree from the Lua value at positive index
// `idx`. Children are fetched by index/key (lua_rawgeti/lua_rawget), so descents
// never disturb a parent's iteration state. On any rejection sets `err` and
// returns false, having allocated only into `arena` -- the caller arena_finishes.
static bool lua_pop_object(lua_State *lstate, int idx, Arena *arena, Object *out, Error *err)
{
  if (!lua_checkstack(lstate, 4)) {
    api_set_error(err, kErrorTypeException, "lua stack exhausted reading a value");
    return false;
  }
  int t = lua_type(lstate, idx);
  if (t != LUA_TTABLE) {
    if (t == LUA_TNIL || t == LUA_TBOOLEAN || t == LUA_TNUMBER || t == LUA_TSTRING) {
      *out = scalarize(lstate, idx, arena);
      return true;
    }
    api_set_error(err, kErrorTypeValidation, "cannot store a %s", lua_typename(lstate, t));
    return false;
  }
  if (!table_init(lstate, idx, arena, out, err)) {
    return false;
  }
  PopFrame stack[kMarshalDepthCap];
  int depth = 0;
  stack[depth++] = (PopFrame){.container = out,
                              .lua_idx = idx,
                              .is_array = out->type == kObjectTypeArray,
                              .n = out->type == kObjectTypeArray ? out->data.array.size
                                                                 : out->data.dict.size,
                              .next = 0,
                              .pop_table = false};
  while (depth > 0) {
    PopFrame *f = &stack[depth - 1];
    if (f->next >= f->n) {
      depth--;
      if (f->pop_table) {
        lua_pop(lstate, 1); // drop this child's source table; the root's is the arg
      }
      continue;
    }
    if (!lua_checkstack(lstate, 4)) {
      api_set_error(err, kErrorTypeException, "lua stack exhausted reading a value");
      return false;
    }
    size_t i = f->next++;
    Object *slot = NULL;
    if (f->is_array) {
      lua_rawgeti(lstate, f->lua_idx, (int)(i + 1));
      slot = &f->container->data.array.items[i];
    } else {
      String key = f->container->data.dict.items[i].key;
      lua_pushlstring(lstate, key.data != NULL ? key.data : "", key.size);
      lua_rawget(lstate, f->lua_idx);
      slot = &f->container->data.dict.items[i].value;
    }
    int ct = lua_type(lstate, -1);
    if (ct == LUA_TTABLE) {
      if (depth >= kMarshalDepthCap) {
        api_set_error(err, kErrorTypeValidation, "table nesting exceeds %d (cyclic table?)",
                      kMarshalDepthCap);
        return false; // child table left on the stack; lua_error unwinds it
      }
      int child_idx = lua_gettop(lstate);
      if (!table_init(lstate, child_idx, arena, slot, err)) {
        return false;
      }
      stack[depth++] = (PopFrame){.container = slot,
                                  .lua_idx = child_idx,
                                  .is_array = slot->type == kObjectTypeArray,
                                  .n = slot->type == kObjectTypeArray ? slot->data.array.size
                                                                      : slot->data.dict.size,
                                  .next = 0,
                                  .pop_table = true};
      // leave the child table on the stack for its frame to index into
    } else if (ct == LUA_TNIL || ct == LUA_TBOOLEAN || ct == LUA_TNUMBER || ct == LUA_TSTRING) {
      *slot = scalarize(lstate, -1, arena);
      lua_pop(lstate, 1);
    } else {
      api_set_error(err, kErrorTypeValidation, "cannot store a %s", lua_typename(lstate, ct));
      lua_pop(lstate, 1);
      return false;
    }
  }
  return true;
}

typedef struct {
  const Object *container; // source Array/Dict
  int lua_idx;             // absolute index of this frame's Lua table
  size_t next;             // next child to emit
} PushFrame;

// Pushes `obj` onto the Lua stack as a value (table for Array/Dict), leaving
// exactly one value on top. Iterative and depth-capped; borrows `obj` (the
// caller frees it). Returns false only on Lua stack exhaustion.
static bool object_to_lua(lua_State *lstate, const Object *obj, Error *err)
{
  if (!lua_checkstack(lstate, 4)) {
    api_set_error(err, kErrorTypeException, "lua stack exhausted writing a value");
    return false;
  }
  if (obj->type != kObjectTypeArray && obj->type != kObjectTypeDict) {
    push_scalar(lstate, obj);
    return true;
  }
  PushFrame stack[kMarshalDepthCap];
  int depth = 0;
  lua_newtable(lstate);
  stack[depth++] = (PushFrame){.container = obj, .lua_idx = lua_gettop(lstate), .next = 0};
  while (depth > 0) {
    PushFrame *f = &stack[depth - 1];
    bool is_array = f->container->type == kObjectTypeArray;
    size_t n = is_array ? f->container->data.array.size : f->container->data.dict.size;
    if (f->next >= n) {
      depth--;
      if (depth > 0) {
        lua_pop(lstate, 1); // child table is filled and attached; drop it (root stays)
      }
      continue;
    }
    if (!lua_checkstack(lstate, 6)) {
      api_set_error(err, kErrorTypeException, "lua stack exhausted writing a value");
      return false;
    }
    size_t i = f->next++;
    String key = STRING_INIT;
    const Object *child;
    if (is_array) {
      child = &f->container->data.array.items[i];
    } else {
      key = f->container->data.dict.items[i].key;
      child = &f->container->data.dict.items[i].value;
    }
    if (child->type == kObjectTypeArray || child->type == kObjectTypeDict) {
      if (depth >= kMarshalDepthCap) {
        api_set_error(err, kErrorTypeException, "object nesting exceeds %d", kMarshalDepthCap);
        return false;
      }
      lua_newtable(lstate); // child table; attach to parent now, keep on stack to fill
      if (is_array) {
        lua_pushvalue(lstate, -1);
        lua_rawseti(lstate, f->lua_idx, (int)(i + 1));
      } else {
        lua_pushlstring(lstate, key.data != NULL ? key.data : "", key.size);
        lua_pushvalue(lstate, -2);
        lua_rawset(lstate, f->lua_idx);
      }
      stack[depth++] = (PushFrame){.container = child, .lua_idx = lua_gettop(lstate), .next = 0};
    } else if (is_array) {
      push_scalar(lstate, child);
      lua_rawseti(lstate, f->lua_idx, (int)(i + 1));
    } else {
      lua_pushlstring(lstate, key.data != NULL ? key.data : "", key.size);
      push_scalar(lstate, child);
      lua_rawset(lstate, f->lua_idx);
    }
  }
  return true;
}

static int l_mua_set_var(lua_State *lstate)
{
  size_t name_len = 0;
  const char *name = luaL_checklstring(lstate, 1, &name_len);
  String name_s = {.data = (char *)name, .size = name_len};
  Arena arena = ARENA_INIT;
  Object value = NIL;
  Error err = ERROR_INIT;
  if (!lua_pop_object(lstate, 2, &arena, &value, &err)) {
    arena_finish(&arena); // free the partial build tree before the longjmp
    return raise_api_error(lstate, &err);
  }
  mua_set_var(name_s, value, &err); // the store deep-copies onto the heap
  arena_finish(&arena);             // release the transient build tree
  if (ERROR_SET(&err)) {
    return raise_api_error(lstate, &err);
  }
  return 0;
}

static int l_mua_get_var(lua_State *lstate)
{
  size_t name_len = 0;
  const char *name = luaL_checklstring(lstate, 1, &name_len);
  String name_s = {.data = (char *)name, .size = name_len};
  Error err = ERROR_INIT;
  Object value = mua_get_var(name_s, &err);
  if (ERROR_SET(&err)) {
    api_free_object(&value);
    return raise_api_error(lstate, &err);
  }
  bool ok = object_to_lua(lstate, &value, &err);
  api_free_object(&value); // free the owned copy once Lua holds its own
  if (!ok) {
    return raise_api_error(lstate, &err);
  }
  return 1;
}

// Registered mechanically; mua.api.mua_* mirrors the C names one-to-one.
static const luaL_Reg api_functions[] = {
  {"mua_set_option", l_mua_set_option},
  {"mua_get_option", l_mua_get_option},
  {"mua_set_var", l_mua_set_var},
  {"mua_get_var", l_mua_get_var},
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
