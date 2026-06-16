#ifndef MUA_API_SESSION_H
#define MUA_API_SESSION_H

#include "mua/api/private/defs.h"

// Session-scoped API -- the neovim nvim_buf_* analog, exposed to Lua as
// mua.api.mua_sess_*. Handles are integers; 0 == the current session (as 0 ==
// the current buffer in nvim). Read-only this slice: the conversation is
// observable, not mutable, from Lua. Resolution goes through session_resolve
// (session.h), so an unknown handle is a clean Validation error.

// Returns the conversation of record for session `sess` as an owned Array of
// message Dicts (free via api_free_object), each in wire shape
// ({role, content, ...}); the injected system prompt is not part of it. The
// Array is a fresh copy of the borrowed conversation, so it outlives any later
// append. An unknown handle sets a Validation error and returns an empty Array.
Array mua_sess_get_messages(Session sess, Error *err) FUNC_API_SINCE(4);

// Returns session `sess`'s id ("YYYYMMDDTHHMMSS_NN") as an owned String (free
// via api_free_string). An unknown handle sets a Validation error and returns
// an empty String.
String mua_sess_get_id(Session sess, Error *err) FUNC_API_SINCE(4);

#endif // MUA_API_SESSION_H
