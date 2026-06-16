local t = require("test.unit.helpers")
local ffi = t.ffi
local lib = t.lib

-- autocmd_event_from_name is the pure, state-free part of the store, so it is
-- FFI-reachable; the rest (create/clear) needs a live Lua state for the ref
-- lifecycle and is covered by the functional autocmd_spec. String comes from the
-- shared helpers cdef.
t.cdef([[
  int autocmd_event_from_name(String name);
]])

-- A length-exact (not NUL-terminated) String view of `str`; the buffer is kept
-- alive by the caller for the duration of the call.
local function name_id(str)
  local buf = ffi.new("char[?]", #str, str)
  local s = ffi.new("String", { data = buf, size = #str })
  return lib.autocmd_event_from_name(s)
end

describe("autocmd_event_from_name", function()
  it("maps the five event names to distinct ids", function()
    local seen = {}
    for _, name in ipairs({ "SessionStart", "SessionEnd", "ToolPre", "ToolPost", "StreamDelta" }) do
      local id = name_id(name)
      assert.is_true(id >= 0)
      assert.is_nil(seen[id]) -- each maps to a distinct event
      seen[id] = name
    end
  end)

  it("returns -1 for an unknown event", function()
    assert.equal(-1, name_id("NoSuchEvent"))
    assert.equal(-1, name_id(""))
  end)

  it("matches by exact length, not prefix", function()
    assert.equal(-1, name_id("ToolPreXYZ")) -- longer than "ToolPre"
    assert.equal(-1, name_id("Tool")) -- shorter
  end)
end)
