#include "mua/api/session.h"

#include <assert.h>

#include "mua/api/private/helpers.h"
#include "mua/json.h"
#include "mua/session.h"

// Session-scoped read accessors. Each resolves the handle through the core
// registry (session.h), then marshals a borrowed view into an owned API value.
// The conversation crosses cJSON -> Object here -- the one place json.h's
// boundary rule permits it -- so the bridge stays cJSON-free.

Array mua_sess_get_messages(Session sess, Error *err)
{
  SessionState *state = session_resolve(sess, err);
  if (state == NULL) {
    return (Array){.items = NULL, .size = 0, .capacity = 0};
  }
  // session_messages is a borrowed cJSON array (valid only until the next
  // append); cjson_to_object copies it into an owned Object tree, so the result
  // is safe to hand to Lua regardless of later appends.
  Object obj = NIL;
  if (!cjson_to_object(session_messages(state), &obj, err)) {
    return (Array){.items = NULL, .size = 0, .capacity = 0};
  }
  assert(obj.type == kObjectTypeArray); // the conversation is always a cJSON array
  // Lift the array payload out of the wrapper (a field read; ownership of the
  // backing storage and children transfers to the caller unchanged).
  return obj.data.array;
}

String mua_sess_get_id(Session sess, Error *err)
{
  SessionState *state = session_resolve(sess, err);
  if (state == NULL) {
    return STRING_INIT;
  }
  return cstr_to_string(session_id(state));
}
