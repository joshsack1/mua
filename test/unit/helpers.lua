assert(jit, "unit tests require busted running under LuaJIT (see README bootstrap)")

local ffi = require("ffi")

local M = {}

M.ffi = ffi

local lib_path = os.getenv("MUA_TEST_LIB") or "./build/lib/libmua.dylib"
M.lib = ffi.load(lib_path)

--- ffi.cdef that tolerates re-definition: spec files share one busted process,
--- and hand-written cdef blocks overlap between specs.
---@param decls string C declarations
function M.cdef(decls)
  local ok, err = pcall(ffi.cdef, decls)
  if not ok and not tostring(err):match("attempt to redefine") then
    error(err, 2)
  end
end

return M
