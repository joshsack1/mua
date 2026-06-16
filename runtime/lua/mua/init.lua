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

return mua
