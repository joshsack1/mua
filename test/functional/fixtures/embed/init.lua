-- Sets a global var so the embed spec can read it back over RPC, proving the one
-- dispatch table drives both Lua (this assignment, via mua.g -> mua_set_var) and
-- the msgpack-RPC server (mua_get_var).
mua.g.greeting = "hi"
