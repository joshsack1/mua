local t = require("test.unit.helpers")
local ffi = t.ffi
local lib = t.lib

-- Declarations mirror src/mua/http.h (loop decls come from helpers). The
-- bridge itself needs a live server to exercise; only the client lifecycle
-- is unit-testable (see the plan's honest-gap note).
t.cdef([[
  typedef struct HttpClient HttpClient;
  HttpClient *http_client_new(uv_loop_t *loop, Error *err);
  void http_client_close(HttpClient *client);
  void http_global_cleanup(void);
]])

describe("http client lifecycle", function()
  it("new / close / drain roundtrip leaks no loop handles", function()
    assert.is_true(lib.loop_init())
    local err = ffi.new("Error")
    err.type = ffi.C.kErrorTypeNone
    local client = lib.http_client_new(lib.loop_get(), err)
    assert.is_true(client ~= nil)
    lib.http_client_close(client)
    -- Drain the deferred timer closes; loop_close fails on leaked handles.
    assert.equal(0, lib.loop_run())
    assert.is_true(lib.loop_close())
  end)

  it("close is idempotent and a second client works after the first", function()
    assert.is_true(lib.loop_init())
    local err = ffi.new("Error")
    err.type = ffi.C.kErrorTypeNone
    local client = lib.http_client_new(lib.loop_get(), err)
    assert.is_true(client ~= nil)
    lib.http_client_close(client)
    lib.http_client_close(client) -- no-op; freed only after drain
    assert.equal(0, lib.loop_run())
    local second = lib.http_client_new(lib.loop_get(), err)
    assert.is_true(second ~= nil)
    lib.http_client_close(second)
    assert.equal(0, lib.loop_run())
    assert.is_true(lib.loop_close())
  end)
end)
