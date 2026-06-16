#ifndef MUA_TOOLS_H
#define MUA_TOOLS_H

#include <stdbool.h>

#include <cjson/cJSON.h>

#include "mua/api/private/defs.h"

// The tool registry and result contract. Tool failures are results the model
// sees (is_error=true), never Error* out of a tool — Error is reserved for
// mechanism failures (a broken schema, not a missing file). This registry
// shape is the seed of the future Lua mua_register_tool.

typedef struct {
  char *content; // xmalloc'd, NUL-terminated; ownership transfers via done
  bool is_error; // the model decides what to do about failures
} ToolResult;

// Fires exactly once per execute, possibly inline (sync tools). `result` is
// borrowed for the call, but ownership of result->content transfers to the
// callee, which must xfree it (typically after copying into a tool message).
typedef void (*ToolDoneCb)(void *ud, const ToolResult *result);

// Opaque async-execution handle (bash). Valid from a non-NULL execute return
// until the moment `done` fires — callers null their reference inside done.
typedef struct ToolExec ToolExec;

typedef struct ToolDef ToolDef;

// One uniform contract: `def` is the looked-up tool (so one shared execute can
// serve every dynamically registered tool, reading its own callback off `def`);
// `args` is the parsed arguments object, BORROWED for the duration of this call
// only (async tools copy what they outlive it with). Returns a cancellable
// handle, or NULL when done already fired inline.
typedef ToolExec *(*ToolExecuteFn)(const ToolDef *def, cJSON *args, ToolDoneCb done, void *ud);

// Built-ins are static const with a `params_schema` string; tools registered
// from Lua are heap entries carrying an owned `schema_json` tree and a `callback`
// LuaRef instead. tools_build_openai_array branches on which schema is set.
struct ToolDef {
  const char *name;
  const char *description;
  const char *params_schema; // builtin: static JSON Schema string; dynamic: NULL
  cJSON *schema_json;        // dynamic: owned schema tree; builtin: NULL
  ToolExecuteFn execute;
  bool mutating; // gate-relevant: read=false; write/edit/bash and (by default) dynamic=true
  LuaRef callback; // dynamic: the Lua callback ref; builtin: unused
};

const ToolDef *tools_lookup(const char *name); // NULL-safe; NULL on unknown

// Registers a Lua-backed tool: takes ownership of `schema` (a wire-shaped JSON
// Schema object) and `callback` (a LuaRef) on success. Sets a Validation error
// (owning neither -- the caller cleans up) on an empty/duplicate name or when
// the registry is full. `name`/`description` are copied.
void tools_register(String name, String description, cJSON *schema, bool mutating,
                    LuaRef callback, Error *err);

// Releases every registered tool (frees names/schemas, unrefs callbacks). Call
// before the Lua state is torn down -- the unref needs a live state.
void tools_teardown(void);

// Fresh wire-shaped "tools" array ([{type:"function",function:{...}}, ...])
// per call; caller owns. NULL + err only on a broken builtin schema.
cJSON *tools_build_openai_array(Error *err);

// Kills a running async tool (SIGKILL). NULL-safe. `done` still fires exactly
// once afterwards, carrying a canceled result — cancellation never suppresses
// the done contract.
void tools_cancel(ToolExec *exec);

#endif // MUA_TOOLS_H
