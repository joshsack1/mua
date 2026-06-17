-- The mua Lua stdlib root. C builds the global `mua` table with its `mua.api`
-- C function surface before this loads (mua_lua_bridge_init), then requires
-- this file to layer the sugar on top and hand the same table back, so
-- require("mua") returns it.
--
-- mua.o is the vim.o-style options proxy and mua.g the vim.g-style globals
-- proxy: empty tables whose metatables forward reads and writes to mua.api, so
-- the real storage stays in C. mua.g holds arbitrary values (incl. tables);
-- mua.g.x = nil deletes the key, and reading an unset key yields nil.
local mua = assert(_G.mua, "mua.api bridge must be installed before requiring mua")

mua.o = setmetatable({}, {
  __index = function(_, key)
    return mua.api.mua_get_option(key)
  end,
  __newindex = function(_, key, value)
    mua.api.mua_set_option(key, value)
  end,
})

mua.g = setmetatable({}, {
  __index = function(_, key)
    return mua.api.mua_get_var(key)
  end,
  __newindex = function(_, key, value)
    mua.api.mua_set_var(key, value)
  end,
})

-- mua.register_tool registers a model-callable tool implemented in Lua. spec
-- fields: name (string), description (string), schema (a JSON Schema table for
-- the args; optional), callback (function(args) -> result), and mutating
-- (optional, default true -> the call goes through the approval gate). The raw
-- mua.api.mua_register_tool is positional (name, description, schema, mutating,
-- callback); this kwargs sugar is the ergonomic form.
function mua.register_tool(spec)
  assert(type(spec) == "table", "mua.register_tool expects a table")
  mua.api.mua_register_tool(spec.name, spec.description, spec.schema, spec.mutating, spec.callback)
end

-- mua.create_autocmd registers a callback fired at a lifecycle event -- one of
-- "SessionStart", "SessionEnd", "ToolPre", "ToolPost", "StreamDelta",
-- "UserPromptPre". opts holds the callback: function(ev) where ev is
-- { event = <name>, ... }. Returns the autocmd id. A ToolPre callback may return
-- false (or a string reason) to veto the tool call, or a table to rewrite the
-- tool's arguments to that table (the rewritten args are what runs, still subject
-- to approval). A UserPromptPre callback (fired before the turn, ev.prompt holds
-- the line) may return false to swallow the prompt (no turn runs) or a string to
-- rewrite it; other events ignore the return. The nvim_create_autocmd shape.
function mua.create_autocmd(event, opts)
  assert(type(opts) == "table", "mua.create_autocmd expects an options table")
  return mua.api.mua_create_autocmd(event, opts.callback)
end

-- mua.sess is the session-scoped read API (the nvim_buf_* analog). The handle
-- defaults to 0 (the current session): get_messages returns the conversation of
-- record as an array of { role = , content = , ... } tables, get_id the session
-- id string. Wraps the raw mua.api.mua_sess_* surface.
mua.sess = {
  get_messages = function(handle)
    return mua.api.mua_sess_get_messages(handle or 0)
  end,
  get_id = function(handle)
    return mua.api.mua_sess_get_id(handle or 0)
  end,
}

return mua
