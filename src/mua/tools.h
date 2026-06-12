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

// One uniform contract: `args` is the parsed arguments object, BORROWED for
// the duration of this call only (async tools copy what they outlive it
// with). Returns a cancellable handle, or NULL when done already fired
// inline.
typedef ToolExec *(*ToolExecuteFn)(cJSON *args, ToolDoneCb done, void *ud);

typedef struct {
  const char *name;
  const char *description;
  const char *params_schema; // static JSON Schema string, parsed per request
  bool mutating;             // gate-relevant: read=false, write/edit/bash=true
  ToolExecuteFn execute;
} ToolDef;

const ToolDef *tools_lookup(const char *name); // NULL-safe; NULL on unknown

// Fresh wire-shaped "tools" array ([{type:"function",function:{...}}, ...])
// per call; caller owns. NULL + err only on a broken builtin schema.
cJSON *tools_build_openai_array(Error *err);

// Kills a running async tool (SIGKILL). NULL-safe. `done` still fires exactly
// once afterwards, carrying a canceled result — cancellation never suppresses
// the done contract.
void tools_cancel(ToolExec *exec);

#endif // MUA_TOOLS_H
