local t = require("test.unit.helpers")
local lib = t.lib

-- Declarations mirror src/mua/loop.h.
t.cdef([[
  typedef struct uv_loop_s uv_loop_t;
  bool loop_init(void);
  uv_loop_t *loop_get(void);
  int loop_run(void);
  void loop_stop(void);
  bool loop_close(void);
]])

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
