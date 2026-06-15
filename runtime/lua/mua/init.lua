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

return mua
