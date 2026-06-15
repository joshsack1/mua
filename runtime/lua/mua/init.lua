-- The mua Lua stdlib root. C builds the global `mua` table with its `mua.api`
-- C function surface before this loads (mua_lua_bridge_init), then requires
-- this file to layer the sugar on top and hand the same table back, so
-- require("mua") returns it.
--
-- mua.o is the vim.o-style options proxy: an empty table whose metatable
-- forwards reads and writes to mua.api, so the real storage stays in C.
local mua = assert(_G.mua, "mua.api bridge must be installed before requiring mua")

mua.o = setmetatable({}, {
  __index = function(_, key)
    return mua.api.mua_get_option(key)
  end,
  __newindex = function(_, key, value)
    mua.api.mua_set_option(key, value)
  end,
})

return mua
