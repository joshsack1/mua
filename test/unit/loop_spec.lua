local t = require("test.unit.helpers")
local lib = t.lib

-- loop.h declarations are shared via helpers (http_spec needs them too).

describe("loop singleton", function()
  it("init / get / run / close roundtrip", function()
    assert.is_true(lib.loop_init())
    assert.is_true(lib.loop_get() ~= nil)
    -- The SIGINT watcher is unref'd, so an idle loop returns immediately.
    assert.equal(0, lib.loop_run())
    assert.is_true(lib.loop_close())
  end)

  it("init is idempotent and close enables re-init", function()
    assert.is_true(lib.loop_init())
    assert.is_true(lib.loop_init())
    assert.is_true(lib.loop_close())
    assert.is_true(lib.loop_init())
    assert.is_true(lib.loop_close())
  end)
end)
