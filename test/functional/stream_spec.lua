local helpers = require("test.functional.helpers")

-- End-to-end specs driving mua against the scripted SSE fixture server: the
-- only automated coverage of the curl<->libuv bridge over real sockets.
-- Boundary hostility here is best-effort (the kernel may coalesce slices);
-- split-invariance itself is owned by the unit suite.

-- Wire builders, mua_env (now isolating MUA_STATE_DIR), and sliced_block are
-- shared via helpers; the agent specs reuse them too.
local SSE_OK = helpers.SSE_OK
local delta = helpers.delta
local FINISH = helpers.FINISH
local DONE = helpers.DONE
local mua_env = helpers.mua_env
local sliced_block = helpers.sliced_block

describe("streaming against the fixture server", function()
  it("happy path with hostile slice boundaries and keep-alive comments", function()
    local wire = SSE_OK
      .. ": OPENROUTER PROCESSING\n\n"
      .. delta("Hel")
      .. delta("lo ")
      .. delta("world")
      .. FINISH
      .. DONE
    local cuts = {
      #SSE_OK - 2, -- between \r\n|\r\n of the response head
      wire:find("lo ", 1, true) + 1, -- mid second delta
      wire:find("[DONE]", 1, true) + 2, -- mid sentinel
    }
    local srv = helpers.start_sse_server({ sliced_block(wire, cuts, "close") })
    local r = helpers.run_mua({ "-p", "say hello" }, mua_env(srv))
    local s = srv.finish()

    assert.equal(0, r.code)
    assert.equal("Hello world\n", r.stdout)
    assert.equal("", r.stderr)
    assert.equal(0, s.code)
    assert.equal(1, #s.requests)
    local req = s.requests[1]
    assert.equal("POST", req.method)
    assert.equal("/chat/completions", req.path)
    assert.equal("Bearer test-key-123", req.headers.authorization)
    assert.equal("application/json", req.headers["content-type"])
    assert.equal("text/event-stream", req.headers.accept)
    assert.equal(#req.body, tonumber(req.headers["content-length"]))
    assert.truthy(req.body:find('"stream":true', 1, true))
    assert.truthy(req.body:find("say hello", 1, true))
  end)

  it("non-200 error envelope is reported and never retried", function()
    local body = '{"error":{"message":"Invalid key"}}'
    local wire = "HTTP/1.1 401 Unauthorized\r\nContent-Type: application/json\r\n"
      .. "Connection: close\r\n\r\n"
      .. body
    local srv = helpers.start_sse_server({ { { "send", wire }, { "close" } } })
    local r = helpers.run_mua({ "-p", "hi" }, mua_env(srv))
    local s = srv.finish()

    assert.equal(1, r.code)
    assert.truthy(r.stderr:match("api error %(HTTP 401%): Invalid key"))
    assert.equal("", r.stdout)
    assert.equal(0, s.code)
    assert.equal(1, #s.requests) -- 4xx is terminal, no retry
  end)

  it("wrong content-type on HTTP 200 is a protocol error", function()
    local wire =
      'HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nConnection: close\r\n\r\n{"ok":true}'
    local srv = helpers.start_sse_server({ { { "send", wire }, { "close" } } })
    local r = helpers.run_mua({ "-p", "hi" }, mua_env(srv))
    local s = srv.finish()

    assert.equal(1, r.code)
    assert.truthy(r.stderr:match("unexpected content%-type"))
    assert.equal(0, s.code)
    assert.equal(1, #s.requests)
  end)

  it("abrupt FIN mid-stream after delivered output: error, no retry", function()
    local wire = SSE_OK .. delta("partial ") .. 'data: {"choi'
    local srv = helpers.start_sse_server({ { { "send", wire }, { "close" } } })
    local r = helpers.run_mua({ "-p", "hi" }, mua_env(srv))
    local s = srv.finish()

    assert.equal(1, r.code)
    assert.truthy(r.stderr:match("stream ended before completion"))
    assert.equal("partial ", r.stdout) -- no cosmetic newline on the error path
    assert.equal(0, s.code)
    assert.equal(1, #s.requests) -- delivered output suppresses the retry
  end)

  it("dropped [DONE] with finish_reason and clean EOF is tolerated", function()
    local wire = SSE_OK .. delta("tolerant") .. FINISH
    local srv = helpers.start_sse_server({ { { "send", wire }, { "close" } } })
    local r = helpers.run_mua({ "-p", "hi" }, mua_env(srv))
    local s = srv.finish()

    assert.equal(0, r.code)
    assert.equal("tolerant\n", r.stdout)
    assert.equal("", r.stderr)
    assert.equal(0, s.code)
    assert.equal(1, #s.requests)
  end)

  it("retries on 503 with Retry-After and succeeds on the next connection", function()
    local errwire = "HTTP/1.1 503 Service Unavailable\r\nContent-Type: application/json\r\n"
      .. "Retry-After: 0\r\nConnection: close\r\n\r\n"
      .. '{"error":{"message":"overloaded"}}'
    local okwire = SSE_OK .. delta("retried") .. FINISH .. DONE
    local srv = helpers.start_sse_server({
      { { "send", errwire }, { "close" } },
      { { "send", okwire }, { "close" } },
    })
    local r = helpers.run_mua({ "-p", "hi" }, mua_env(srv))
    local s = srv.finish()

    assert.equal(0, r.code)
    assert.equal("retried\n", r.stdout)
    assert.equal("", r.stderr)
    assert.equal(0, s.code)
    assert.equal(2, #s.requests)
    assert.equal(s.requests[1].body, s.requests[2].body) -- identical resend
  end)

  it("retries on a connection reset before any response bytes", function()
    local okwire = SSE_OK .. delta("recovered") .. FINISH .. DONE
    local srv = helpers.start_sse_server({
      { { "reset" } },
      { { "send", okwire }, { "close" } },
    })
    local r = helpers.run_mua({ "-p", "hi" }, mua_env(srv))
    local s = srv.finish()

    assert.equal(0, r.code)
    assert.equal("recovered\n", r.stdout)
    assert.equal(0, s.code)
    assert.equal(2, #s.requests)
  end)
end)
