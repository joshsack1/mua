#ifndef MUA_API_DISPATCH_H
#define MUA_API_DISPATCH_H

#include <stddef.h>

#include "mua/api/private/defs.h"

// The single source of truth for the marshalable API surface. Every entry adapts
// a public mua_* function to one uniform shape -- arguments arrive as an Object
// Array, the result leaves as an owned Object, errors via `err` -- so the same
// table drives both the Lua bridge (bridge.c) and the --embed msgpack-RPC
// dispatcher (rpc.c). This is what the FUNC_API_SINCE annotations always fed.
//
// Excluded: functions taking a LuaRef (mua_register_tool, mua_create_autocmd) --
// a callback cannot cross RPC -- and Lua-only helpers (mua_clear_autocmds). Those
// stay bespoke Lua wrappers.

typedef Object (*ApiDispatchFn)(Array args, Error *err);

typedef struct {
  const char *name; // the mua_* method name
  ApiDispatchFn fn; // arity/type-checks args, calls the API fn, returns an owned Object
  int since;        // the FUNC_API_SINCE level
} ApiFnMeta;

// The table of marshalable API functions; `count` receives the entry count.
const ApiFnMeta *api_dispatch_table(size_t *count);

// Looks up an entry by name (`name` need not be NUL-terminated; matched against
// `len` bytes exactly). NULL when unknown.
const ApiFnMeta *api_dispatch_find(const char *name, size_t len);

#endif // MUA_API_DISPATCH_H
