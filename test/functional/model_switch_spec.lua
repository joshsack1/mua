local helpers = require("test.functional.helpers")

-- End-to-end coverage of mid-session model switching. The REPL re-resolves
-- mua.o.model at the start of each turn (after UserPromptPre), so a hook can
-- change the model between turns; CLI -m locks it for the whole session. The
-- fixture's hook switches to "model-b" on "/model B" (swallowing that turn) and
-- on "switch-and-run" (letting the turn run, for the -p case). We assert on the
-- "model" field of each captured request body. mua_env sets MUA_CONTEXT_LENGTH=0,
-- which disables the budget and the startup /models fetch, keeping it deterministic.

local FIX = "test/functional/fixtures/model_switch"

-- Two connections, each returning one text response (one per REPL turn).
local function two_texts(a, b)
  return {
    { { "send", helpers.text_block(a) }, { "close" } },
    { { "send", helpers.text_block(b) }, { "close" } },
  }
end

local function one_text(a)
  return { { { "send", helpers.text_block(a) }, { "close" } } }
end

local function run(args, stdin, blocks)
  local dir = helpers.tmpdir()
  local srv = helpers.start_sse_server(blocks)
  local env = helpers.mua_env(srv, dir)
  env.MUA_CONFIG_DIR = FIX
  local r = helpers.run_mua(args, env, stdin and { stdin = stdin } or nil)
  local s = srv.finish()
  helpers.rm_rf(dir)
  return r, s
end

describe("mid-session model switch", function()
  it("re-resolves mua.o.model per turn so a hook can switch it", function()
    -- hello -> turn 1 on model-a; /model B -> hook swallows + switches;
    -- hello again -> turn 2 on model-b.
    local r, s = run({}, "hello\n/model B\nhello again\nexit\n", two_texts("one", "two"))

    assert.equal(0, r.code)
    assert.equal(2, #s.requests)
    assert.truthy(s.requests[1].body:find('"model":"model-a"', 1, true))
    assert.falsy(s.requests[1].body:find('"model":"model-b"', 1, true))
    assert.truthy(s.requests[2].body:find('"model":"model-b"', 1, true)) -- switched
  end)

  it("CLI -m locks the model for the whole session", function()
    local r, s = run({ "-m", "model-cli" }, "hello\n/model B\nhello again\nexit\n", two_texts("one", "two"))

    assert.equal(0, r.code)
    assert.equal(2, #s.requests)
    assert.truthy(s.requests[1].body:find('"model":"model-cli"', 1, true))
    assert.truthy(s.requests[2].body:find('"model":"model-cli"', 1, true))
    assert.falsy(s.requests[2].body:find('"model":"model-b"', 1, true)) -- the hook's set is ignored
  end)

  it("a normal turn uses the configured launch model (passthrough)", function()
    local r, s = run({}, "hello\nexit\n", one_text("one"))

    assert.equal(0, r.code)
    assert.equal(1, #s.requests)
    assert.truthy(s.requests[1].body:find('"model":"model-a"', 1, true))
  end)

  it("-p re-resolves the model after UserPromptPre (no use-after-free)", function()
    -- The startup borrow points into the store (init.lua set mua.o.model), then
    -- the hook reassigns it, freeing that copy. Re-resolving after the hook both
    -- reflects the switch and avoids reading the freed pointer -- the ASan guard.
    local r, s = run({ "-p", "switch-and-run" }, nil, one_text("done"))

    assert.equal(0, r.code)
    assert.equal(1, #s.requests)
    assert.truthy(s.requests[1].body:find('"model":"model-b"', 1, true))
  end)

  it("fetches the /models catalog once and reuses it across a switch", function()
    -- One block per connection, in order: the startup GET /models, then turn 1
    -- (model-a), then turn 2 (model-b). A working cache means the switch does NOT
    -- trigger a second catalog fetch -- exactly one GET /models among the requests.
    local dir = helpers.tmpdir()
    local catalog = '{"data":['
      .. '{"id":"model-a","context_length":128000},'
      .. '{"id":"model-b","context_length":200000}]}'
    local models_block = {
      {
        "send",
        "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nConnection: close\r\n\r\n" .. catalog,
      },
      { "close" },
    }
    local function turn(t)
      return { { "send", helpers.text_block(t) }, { "close" } }
    end
    local srv = helpers.start_sse_server({ models_block, turn("one"), turn("two") }, { timeout_ms = 4000 })
    local env = helpers.mua_env(srv, dir)
    env.MUA_CONFIG_DIR = FIX
    env.MUA_CONTEXT_LENGTH = nil -- enable the startup catalog fetch (mua_env sets "0" otherwise)
    local r = helpers.run_mua({}, env, { stdin = "hello\n/model B\nhello again\nexit\n" })
    local s = srv.finish()
    helpers.rm_rf(dir)

    assert.equal(0, r.code)
    assert.equal(3, #s.requests) -- one /models + two turns; the switch did not refetch
    local models_fetches = 0
    for _, req in ipairs(s.requests) do
      if req.method == "GET" and req.path == "/models" then
        models_fetches = models_fetches + 1
      end
    end
    assert.equal(1, models_fetches) -- catalog fetched once, reused across the switch
    assert.truthy(s.requests[2].body:find('"model":"model-a"', 1, true))
    assert.truthy(s.requests[3].body:find('"model":"model-b"', 1, true))
  end)
end)
