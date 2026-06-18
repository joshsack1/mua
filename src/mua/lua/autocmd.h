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

// ToolPre: payload { event, tool, args }. The only veto/approve/rewrite event --
// a hook returning false vetoes (reason left NULL for the caller to generic-fill),
// a string vetoes with that reason (xmalloc'd into *reason_out, caller frees),
// boolean true approves the call outright (the caller skips the base gate, so no
// y/N prompt fires), and a table rewrites the args to that table. Returns whether
// the call was vetoed; *approve_out is set when a hook approved and none vetoed (a
// veto always wins); on an un-vetoed rewrite *rewrite_out gets a heap Object (the
// caller owns it via api_free_object), else *rewrite_out is left NIL. `args` is
// borrowed.
bool mua_lua_autocmd_tool_pre(const char *name, Object args, Object *rewrite_out, bool *approve_out,
                              char **reason_out);

// ToolPost: payload { event, tool, content, error }.
void mua_lua_autocmd_tool_post(const char *name, bool is_error, String content);

// UserPromptPre: payload { event, prompt }. Fires after a prompt is read and
// before the turn is built -- the input-side analog of ToolPre. A hook returning
// false swallows the prompt (no turn runs; the hook prints any user-facing
// message itself), a string rewrites the prompt to that string (chaining to later
// hooks), and any other return allows it unchanged. Returns whether the prompt was
// swallowed; on an un-swallowed rewrite *rewrite_out gets an xmalloc'd string the
// caller frees (xfree), else *rewrite_out is left NULL. `prompt` is borrowed.
bool mua_lua_autocmd_user_prompt_pre(const char *prompt, char **rewrite_out);

#endif // MUA_LUA_AUTOCMD_H
