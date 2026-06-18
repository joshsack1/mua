#include "mua/lua/bridge.h"

#include <stdio.h>
#include <string.h>

#include <lauxlib.h>

#include "mua/api/dispatch.h"
#include "mua/api/global.h"
#include "mua/api/private/helpers.h"
#include "mua/api/session.h"
#include "mua/autocmd.h"
#include "mua/log.h"
#include "mua/lua/autocmd.h"
#include "mua/lua/ref.h"
#include "mua/lua/state.h"
#include "mua/lua/tool.h"
#include "mua/memory.h"

// The C<->Lua bridge: it exposes the public C API mechanically as mua.api.mua_*
// (identical names and argument order) and layers the mua.o sugar on top. All
// option storage lives in C; this layer only marshals values across and
// translates API errors into Lua errors.

// Raise a Lua error carrying `err`'s message, freeing the C error first:
// lua_error longjmps and never returns, so a clear placed after it would leak.
static int raise_api_error(lua_State *lstate, Error *err)
{
  lua_pushstring(lstate, err->msg != NULL ? err->msg : "api error");
  api_clear_error(err);
  return lua_error(lstate);
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
      if (num >= -MUA_EXACT_INT_MAX && num <= MUA_EXACT_INT_MAX && num == (double)(Integer)num) {
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
  stack[depth++] =
    (PopFrame){.container = out,
               .lua_idx = idx,
               .is_array = out->type == kObjectTypeArray,
               .n = out->type == kObjectTypeArray ? out->data.array.size : out->data.dict.size,
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

// Maximum positional args any API function takes (set_*: name + value).
enum { kMaxApiArgs = 8 };

// The one Lua entry point for every table-driven API function: marshal the stack
// args into an Object Array, invoke the metadata's dispatch fn, and push the
// owned result. The closure's upvalue is the function's index into
// api_dispatch_table(). The RPC server feeds the same fn, so the surface stays
// single-sourced; here the only extra work is Lua<->Object marshaling.
static int l_api_dispatch(lua_State *lstate)
{
  size_t count = 0;
  const ApiFnMeta *table = api_dispatch_table(&count);
  const ApiFnMeta *meta = &table[(size_t)lua_tointeger(lstate, lua_upvalueindex(1))];
  int argc = lua_gettop(lstate);
  if (argc > kMaxApiArgs) {
    return luaL_error(lstate, "%s: too many arguments", meta->name);
  }
  Arena arena = ARENA_INIT;
  Object items[kMaxApiArgs];
  Error err = ERROR_INIT;
  for (int i = 0; i < argc; i++) {
    if (!lua_pop_object(lstate, i + 1, &arena, &items[i], &err)) {
      arena_finish(&arena); // free the partial build tree before the longjmp
      return raise_api_error(lstate, &err);
    }
  }
  Array args = {.items = items, .size = (size_t)argc, .capacity = (size_t)argc};
  Object result = meta->fn(args, &err);
  arena_finish(&arena); // the API copied what it kept; free the marshaled args
  if (ERROR_SET(&err)) {
    api_free_object(&result); // NIL by convention on error; freed defensively
    return raise_api_error(lstate, &err);
  }
  bool ok = object_to_lua(lstate, &result, &err);
  api_free_object(&result); // free the owned copy once Lua holds its own
  if (!ok) {
    return raise_api_error(lstate, &err);
  }
  return 1;
}

// mua.api.mua_register_tool(name, description, schema, mutating, callback).
// schema is a table (the JSON Schema for the tool's args) or nil; callback is
// the implementing function. Held via luaL_ref; the registry owns it.
static int l_mua_register_tool(lua_State *lstate)
{
  size_t name_len = 0;
  const char *name = luaL_checklstring(lstate, 1, &name_len);
  size_t desc_len = 0;
  const char *desc = luaL_optlstring(lstate, 2, "", &desc_len);
  luaL_checktype(lstate, 5, LUA_TFUNCTION); // validate before allocating or ref'ing
  // Schema (arg 3): a table -> Object, nil -> Nil (defaulted downstream). Built
  // into an arena so a marshaling error frees cleanly across the lua_error longjmp.
  Arena arena = ARENA_INIT;
  Object schema = NIL;
  Error err = ERROR_INIT;
  if (!lua_isnoneornil(lstate, 3) && !lua_pop_object(lstate, 3, &arena, &schema, &err)) {
    arena_finish(&arena);
    return raise_api_error(lstate, &err);
  }
  bool mutating = lua_isnoneornil(lstate, 4) ? true : (lua_toboolean(lstate, 4) != 0);
  lua_pushvalue(lstate, 5);
  LuaRef callback = luaL_ref(lstate, LUA_REGISTRYINDEX); // pops the pushed copy
  String name_s = {.data = (char *)name, .size = name_len};
  String desc_s = {.data = (char *)desc, .size = desc_len};
  mua_register_tool(name_s, desc_s, schema, mutating, callback, &err);
  arena_finish(&arena); // the schema was copied to cJSON (or defaulted) downstream
  if (ERROR_SET(&err)) {
    luaL_unref(lstate, LUA_REGISTRYINDEX, callback); // registration failed; release the ref
    return raise_api_error(lstate, &err);
  }
  return 0;
}

// mua.api.mua_create_autocmd(event, callback) -> id (1:1 with the C signature;
// the nvim-style { callback = fn } opts table is the sugar in init.lua). The
// store owns the ref. Returns the autocmd id.
static int l_mua_create_autocmd(lua_State *lstate)
{
  size_t event_len = 0;
  const char *event_name = luaL_checklstring(lstate, 1, &event_len);
  luaL_checktype(lstate, 2, LUA_TFUNCTION);
  lua_pushvalue(lstate, 2);
  LuaRef callback = luaL_ref(lstate, LUA_REGISTRYINDEX); // pops the pushed copy
  String event_s = {.data = (char *)event_name, .size = event_len};
  Error err = ERROR_INIT;
  Integer id = mua_create_autocmd(event_s, callback, &err);
  if (ERROR_SET(&err)) {
    luaL_unref(lstate, LUA_REGISTRYINDEX, callback); // unknown event / full store: release
    return raise_api_error(lstate, &err);
  }
  lua_pushinteger(lstate, (lua_Integer)id);
  return 1;
}

static int l_mua_clear_autocmds(lua_State *lstate)
{
  (void)lstate;
  mua_clear_autocmds();
  return 0;
}

// --- Registered-tool callback seam (declared in lua/tool.h) -------------------
// tools.c (Lua-agnostic) calls these to run and release a Lua tool callback.
// They speak Object only -- the cJSON<->Object conversion stays in tools.c -- so
// cJSON never reaches this layer.

// An owned String Object copied from `data`/`len` (NUL not required).
static Object owned_string(const char *data, size_t len)
{
  return STRING_OBJ(api_string_dup((String){.data = (char *)data, .size = len}));
}

static Object owned_cstr(const char *str)
{
  return owned_string(str, str != NULL ? strlen(str) : 0);
}

void mua_lua_tool_invoke(LuaRef callback, Object args, Object *result_out, bool *is_error_out)
{
  lua_State *lstate = mua_lua_state();
  *result_out = NIL;
  *is_error_out = false;
  if (!lua_checkstack(lstate, 4)) {
    *result_out = owned_cstr("tool callback: lua stack exhausted");
    *is_error_out = true;
    return;
  }
  lua_rawgeti(lstate, LUA_REGISTRYINDEX, callback); // the stored callback function
  Error err = ERROR_INIT;
  if (!object_to_lua(lstate, &args, &err)) {
    lua_pop(lstate, 1); // drop the function; nothing was called
    *result_out = owned_cstr(err.msg != NULL ? err.msg : "tool args could not be marshaled");
    *is_error_out = true;
    api_clear_error(&err);
    return;
  }
  if (lua_pcall(lstate, 1, 1, 0) != 0) {
    size_t len = 0;
    const char *msg = lua_tolstring(lstate, -1, &len); // error object coerced to a string
    *result_out = (msg != NULL) ? owned_string(msg, len) : owned_cstr("tool callback raised");
    *is_error_out = true;
    lua_pop(lstate, 1);
    return;
  }
  switch (lua_type(lstate, -1)) {
    case LUA_TSTRING: {
      size_t len = 0;
      const char *str = lua_tolstring(lstate, -1, &len);
      *result_out = owned_string(str, len); // returned verbatim as the content
      break;
    }
    case LUA_TNIL:
      *result_out = owned_string("", 0); // no output -> empty content
      break;
    default: {
      // Any other value: marshal it to an Object for the caller to JSON-encode.
      Arena arena = ARENA_INIT;
      Object built = NIL;
      if (lua_pop_object(lstate, lua_gettop(lstate), &arena, &built, &err)) {
        *result_out = api_copy_object(&built); // heap-own; the arena build was transient
      } else {
        *result_out = owned_cstr(err.msg != NULL ? err.msg : "tool result could not be marshaled");
        *is_error_out = true;
        api_clear_error(&err);
      }
      arena_finish(&arena);
      break;
    }
  }
  lua_pop(lstate, 1); // drop the result
}

void mua_lua_unref(LuaRef callback)
{
  luaL_unref(mua_lua_state(), LUA_REGISTRYINDEX, callback);
}

// --- Autocmd dispatch seam (declared in lua/autocmd.h) ------------------------
// Each fires every callback of an event with one payload table. cJSON never
// reaches here: callers pass primitives / already-marshaled Objects. A throwing
// hook is logged and skipped (nonfatal); a slow hook blocks the loop (the Lua
// VM is single-threaded), as with tool callbacks.

// Pushes a fresh payload table { event = <name> } and returns its stack index.
static int autocmd_payload(lua_State *lstate, const char *event_name)
{
  lua_newtable(lstate);
  int tidx = lua_gettop(lstate);
  lua_pushstring(lstate, event_name);
  lua_setfield(lstate, tidx, "event");
  return tidx;
}

// Fires every callback of `event` with the payload table on top (left in place
// for the caller to pop). Discards results; logs and continues on a throw.
static void autocmd_fire(lua_State *lstate, AutocmdEvent event, const char *event_name)
{
  size_t n = autocmd_count(event);
  for (size_t i = 0; i < n; i++) {
    lua_rawgeti(lstate, LUA_REGISTRYINDEX, autocmd_ref_at(event, i));
    lua_pushvalue(lstate, -2); // a fresh reference to the payload table
    if (lua_pcall(lstate, 1, 0, 0) != 0) {
      const char *msg = lua_tostring(lstate, -1);
      log_msg(kLogWarn, "autocmd: %s hook error: %s", event_name, msg != NULL ? msg : "?");
      lua_pop(lstate, 1); // the error object
    }
  }
}

void mua_lua_autocmd_session(AutocmdEvent event, const char *session_id)
{
  if (autocmd_count(event) == 0) {
    return;
  }
  const char *name = (event == kAutocmdSessionStart) ? "SessionStart" : "SessionEnd";
  lua_State *lstate = mua_lua_state();
  if (!lua_checkstack(lstate, 4)) {
    return;
  }
  int tidx = autocmd_payload(lstate, name);
  lua_pushstring(lstate, session_id != NULL ? session_id : "");
  lua_setfield(lstate, tidx, "session");
  autocmd_fire(lstate, event, name);
  lua_pop(lstate, 1); // the payload table
}

void mua_lua_autocmd_stream_delta(String text)
{
  if (autocmd_count(kAutocmdStreamDelta) == 0) {
    return;
  }
  lua_State *lstate = mua_lua_state();
  if (!lua_checkstack(lstate, 4)) {
    return;
  }
  int tidx = autocmd_payload(lstate, "StreamDelta");
  lua_pushlstring(lstate, text.data != NULL ? text.data : "", text.size);
  lua_setfield(lstate, tidx, "text");
  autocmd_fire(lstate, kAutocmdStreamDelta, "StreamDelta");
  lua_pop(lstate, 1);
}

void mua_lua_autocmd_tool_post(const char *name, bool is_error, String content)
{
  if (autocmd_count(kAutocmdToolPost) == 0) {
    return;
  }
  lua_State *lstate = mua_lua_state();
  if (!lua_checkstack(lstate, 4)) {
    return;
  }
  int tidx = autocmd_payload(lstate, "ToolPost");
  lua_pushstring(lstate, name != NULL ? name : "");
  lua_setfield(lstate, tidx, "tool");
  lua_pushlstring(lstate, content.data != NULL ? content.data : "", content.size);
  lua_setfield(lstate, tidx, "content");
  lua_pushboolean(lstate, is_error);
  lua_setfield(lstate, tidx, "error");
  autocmd_fire(lstate, kAutocmdToolPost, "ToolPost");
  lua_pop(lstate, 1);
}

// Reads the (possibly hook-mutated) payload `args` field back into a heap Object
// the caller owns. Mirrors mua_lua_tool_invoke's pop-copy-finish: the
// lua_pop_object build is arena-transient, so api_copy_object lifts it to the heap.
static Object capture_tool_pre_args(lua_State *lstate, int tidx)
{
  lua_getfield(lstate, tidx, "args");
  Arena arena = ARENA_INIT;
  Object built = NIL;
  Object out = NIL;
  Error err = ERROR_INIT;
  if (lua_pop_object(lstate, lua_gettop(lstate), &arena, &built, &err)) {
    out = api_copy_object(&built); // heap-own; the arena build was transient
  } else {
    api_clear_error(&err);
  }
  arena_finish(&arena);
  lua_pop(lstate, 1); // the args field value
  return out;
}

bool mua_lua_autocmd_tool_pre(const char *name, Object args, Object *rewrite_out, bool *approve_out,
                              char **reason_out)
{
  *reason_out = NULL;
  *rewrite_out = NIL;
  *approve_out = false;
  size_t n = autocmd_count(kAutocmdToolPre);
  if (n == 0) {
    return false;
  }
  lua_State *lstate = mua_lua_state();
  if (!lua_checkstack(lstate, 6)) {
    log_msg(kLogWarn, "autocmd: ToolPre skipped (lua stack exhausted)");
    return false;
  }
  int tidx = autocmd_payload(lstate, "ToolPre");
  lua_pushstring(lstate, name != NULL ? name : "");
  lua_setfield(lstate, tidx, "tool");
  Error err = ERROR_INIT;
  if (object_to_lua(lstate, &args, &err)) {
    lua_setfield(lstate, tidx, "args");
  } else {
    api_clear_error(&err);
    lua_settop(lstate, tidx); // discard any partial value object_to_lua left
    lua_pushnil(lstate);
    lua_setfield(lstate, tidx, "args");
  }
  bool vetoed = false;
  bool rewritten = false;
  bool approved = false; // a hook returned boolean true: approve, skip the base gate
  for (size_t i = 0; i < n && !vetoed; i++) {
    lua_rawgeti(lstate, LUA_REGISTRYINDEX, autocmd_ref_at(kAutocmdToolPre, i));
    lua_pushvalue(lstate, tidx);
    if (lua_pcall(lstate, 1, 1, 0) != 0) {
      const char *msg = lua_tostring(lstate, -1);
      log_msg(kLogWarn, "autocmd: ToolPre hook error: %s", msg != NULL ? msg : "?");
      lua_pop(lstate, 1);
      continue;
    }
    // string -> veto reason; false -> veto generic; true -> approve (skip the
    // base gate); table -> rewrite args; else (nil, ...) defer to the base gate.
    int rt = lua_type(lstate, -1);
    if (rt == LUA_TSTRING) {
      size_t rlen = 0;
      const char *reason = lua_tolstring(lstate, -1, &rlen);
      *reason_out = xstrndup(reason, rlen);
      vetoed = true;
      lua_pop(lstate, 1);
    } else if (rt == LUA_TBOOLEAN) {
      if (lua_toboolean(lstate, -1)) {
        approved = true; // true -> approve outright; a later veto still overrides
      } else {
        vetoed = true; // false -> veto; reason left NULL, the caller generic-fills
      }
      lua_pop(lstate, 1);
    } else if (rt == LUA_TTABLE) {
      lua_setfield(lstate, tidx, "args"); // update the payload so later hooks chain
      rewritten = true;                   // (setfield pops the returned table)
    } else {
      lua_pop(lstate, 1); // allow; discard the return value
    }
  }
  if (!vetoed && rewritten) {
    *rewrite_out = capture_tool_pre_args(lstate, tidx);
  }
  *approve_out = approved && !vetoed; // a veto (even after an approve) wins
  lua_pop(lstate, 1);                 // the payload table
  return vetoed;
}

bool mua_lua_autocmd_user_prompt_pre(const char *prompt, char **rewrite_out)
{
  *rewrite_out = NULL;
  size_t n = autocmd_count(kAutocmdUserPromptPre);
  if (n == 0) {
    return false;
  }
  lua_State *lstate = mua_lua_state();
  if (!lua_checkstack(lstate, 6)) {
    log_msg(kLogWarn, "autocmd: UserPromptPre skipped (lua stack exhausted)");
    return false;
  }
  int tidx = autocmd_payload(lstate, "UserPromptPre");
  lua_pushstring(lstate, prompt != NULL ? prompt : "");
  lua_setfield(lstate, tidx, "prompt");
  bool swallowed = false;
  bool rewritten = false;
  for (size_t i = 0; i < n && !swallowed; i++) {
    lua_rawgeti(lstate, LUA_REGISTRYINDEX, autocmd_ref_at(kAutocmdUserPromptPre, i));
    lua_pushvalue(lstate, tidx);
    if (lua_pcall(lstate, 1, 1, 0) != 0) {
      const char *msg = lua_tostring(lstate, -1);
      log_msg(kLogWarn, "autocmd: UserPromptPre hook error: %s", msg != NULL ? msg : "?");
      lua_pop(lstate, 1);
      continue;
    }
    // string -> rewrite the prompt; false -> swallow (skip the turn); else allow.
    int rt = lua_type(lstate, -1);
    if (rt == LUA_TSTRING) {
      lua_setfield(lstate, tidx, "prompt"); // update the payload so later hooks chain
      rewritten = true;                     // (setfield pops the returned string)
    } else if (rt == LUA_TBOOLEAN && !lua_toboolean(lstate, -1)) {
      swallowed = true;
      lua_pop(lstate, 1);
    } else {
      lua_pop(lstate, 1); // allow; discard the return value
    }
  }
  if (!swallowed && rewritten) {
    lua_getfield(lstate, tidx, "prompt");
    size_t len = 0;
    const char *s = lua_tolstring(lstate, -1, &len);
    *rewrite_out = xstrndup(s != NULL ? s : "", len);
    lua_pop(lstate, 1); // the prompt field value
  }
  lua_pop(lstate, 1); // the payload table
  return swallowed;
}

// The Lua-only functions, registered as-is: register_tool/create_autocmd take a
// LuaRef (a callback can't cross RPC), and clear_autocmds is a teardown helper
// with no Error/since -- none belong in the shared dispatch table.
static const luaL_Reg bespoke_functions[] = {
  {"mua_register_tool", l_mua_register_tool},
  {"mua_create_autocmd", l_mua_create_autocmd},
  {"mua_clear_autocmds", l_mua_clear_autocmds},
  {NULL, NULL},
};

bool mua_lua_bridge_init(lua_State *lstate)
{
  lua_newtable(lstate); // the `mua` table
  lua_newtable(lstate); // the `mua.api` subtable
  // The marshalable API functions: one generic dispatch closure per entry, the
  // function's table index carried as an upvalue. The same table drives RPC.
  size_t count = 0;
  const ApiFnMeta *table = api_dispatch_table(&count);
  for (size_t i = 0; i < count; i++) {
    lua_pushinteger(lstate, (lua_Integer)i);
    lua_pushcclosure(lstate, l_api_dispatch, 1);
    lua_setfield(lstate, -2, table[i].name);
  }
  for (const luaL_Reg *fn = bespoke_functions; fn->name != NULL; fn++) {
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
