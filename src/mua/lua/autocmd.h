#ifndef MUA_LUA_AUTOCMD_H
#define MUA_LUA_AUTOCMD_H

#include <stdbool.h>

#include "mua/api/private/defs.h"
#include "mua/autocmd.h"

// The dispatch side of the autocmd store: the frontend (main.c) calls these at
// the agent's callback seams. They marshal the event payload Object->Lua and
// invoke each registered callback with one event table, nonfatal (a throwing
// hook is logged and skipped). Implemented in the bridge so cJSON never crosses
// into the Lua layer -- the caller passes already-marshaled Objects/primitives.
// Each is a single count-check when no hook is registered for the event.

// SessionStart / SessionEnd: payload { event, session = <id> }.
void mua_lua_autocmd_session(AutocmdEvent event, const char *session_id);

// StreamDelta: payload { event, text }. Hot path (per provider chunk).
void mua_lua_autocmd_stream_delta(String text);

// ToolPre: payload { event, tool, args }. The only veto-capable event -- a hook
// returning false vetoes (reason left NULL for the caller to generic-fill), a
// string vetoes with that reason (xmalloc'd into *reason_out, caller frees).
// Returns whether the call was vetoed. `args` is borrowed.
bool mua_lua_autocmd_tool_pre(const char *name, Object args, char **reason_out);

// ToolPost: payload { event, tool, content, error }.
void mua_lua_autocmd_tool_post(const char *name, bool is_error, String content);

#endif // MUA_LUA_AUTOCMD_H
