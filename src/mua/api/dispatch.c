#include "mua/api/dispatch.h"

#include <string.h>

#include "mua/api/global.h"
#include "mua/api/private/helpers.h"
#include "mua/api/session.h"

// Uniform Object-Array wrappers over the public API. Each validates arity and
// argument types (a mismatch is a Validation error, never a crash), then adapts
// to the real C signature. The API copies the inputs it keeps, so the borrowed
// `args` stay owned by the caller, which frees them after the call. On error a
// wrapper returns NIL; the result is otherwise an owned Object.

static Object dispatch_set_option(Array args, Error *err)
{
  if (args.size != 2 || args.items[0].type != kObjectTypeString) {
    api_set_error(err, kErrorTypeValidation, "mua_set_option expects (string name, value)");
    return NIL;
  }
  mua_set_option(args.items[0].data.string, args.items[1], err);
  return NIL;
}

static Object dispatch_get_option(Array args, Error *err)
{
  if (args.size != 1 || args.items[0].type != kObjectTypeString) {
    api_set_error(err, kErrorTypeValidation, "mua_get_option expects (string name)");
    return NIL;
  }
  return mua_get_option(args.items[0].data.string, err);
}

static Object dispatch_set_var(Array args, Error *err)
{
  if (args.size != 2 || args.items[0].type != kObjectTypeString) {
    api_set_error(err, kErrorTypeValidation, "mua_set_var expects (string name, value)");
    return NIL;
  }
  mua_set_var(args.items[0].data.string, args.items[1], err);
  return NIL;
}

static Object dispatch_get_var(Array args, Error *err)
{
  if (args.size != 1 || args.items[0].type != kObjectTypeString) {
    api_set_error(err, kErrorTypeValidation, "mua_get_var expects (string name)");
    return NIL;
  }
  return mua_get_var(args.items[0].data.string, err);
}

// The session accessors take an optional integer handle (0 == current), so 0 or
// 1 args are accepted, mirroring the Lua side's luaL_optinteger(1, 0).
static bool sess_handle(Array args, Session *out, Error *err)
{
  if (args.size == 0) {
    *out = 0;
    return true;
  }
  if (args.size == 1 && args.items[0].type == kObjectTypeInteger) {
    *out = (Session)args.items[0].data.integer;
    return true;
  }
  api_set_error(err, kErrorTypeValidation, "expects an optional integer session handle");
  return false;
}

static Object dispatch_sess_get_messages(Array args, Error *err)
{
  Session handle = 0;
  if (!sess_handle(args, &handle, err)) {
    return NIL;
  }
  Array messages = mua_sess_get_messages(handle, err);
  if (ERROR_SET(err)) {
    return NIL; // messages is empty on error -- nothing to free
  }
  return ARRAY_OBJ(messages);
}

static Object dispatch_sess_get_id(Array args, Error *err)
{
  Session handle = 0;
  if (!sess_handle(args, &handle, err)) {
    return NIL;
  }
  String id = mua_sess_get_id(handle, err);
  if (ERROR_SET(err)) {
    return NIL;
  }
  return STRING_OBJ(id);
}

static const ApiFnMeta g_api_table[] = {
  {"mua_set_option", dispatch_set_option, 1},
  {"mua_get_option", dispatch_get_option, 1},
  {"mua_set_var", dispatch_set_var, 2},
  {"mua_get_var", dispatch_get_var, 2},
  {"mua_sess_get_messages", dispatch_sess_get_messages, 4},
  {"mua_sess_get_id", dispatch_sess_get_id, 4},
};

const ApiFnMeta *api_dispatch_table(size_t *count)
{
  *count = sizeof g_api_table / sizeof g_api_table[0];
  return g_api_table;
}

const ApiFnMeta *api_dispatch_find(const char *name, size_t len)
{
  size_t count = sizeof g_api_table / sizeof g_api_table[0];
  for (size_t i = 0; i < count; i++) {
    if (strlen(g_api_table[i].name) == len && memcmp(g_api_table[i].name, name, len) == 0) {
      return &g_api_table[i];
    }
  }
  return NULL;
}
