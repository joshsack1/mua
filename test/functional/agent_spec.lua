local helpers = require("test.functional.helpers")

-- The agent loop end to end against the fixture: tool round-trips, the gate,
-- the REPL, and the step cap. No API key; MUA_SYSTEM_PROMPT="" (via mua_env)
-- keeps captured request bodies free of the injected system message.

local function tool_then_text(call, text)
  -- Two connections: block 1 returns a tool call, block 2 returns final text.
  return {
    { { "send", helpers.tool_call_block({ call }) }, { "close" } },
    { { "send", helpers.text_block(text) }, { "close" } },
  }
end

describe("agent tool round-trips", function()
  it("runs an approved bash tool and feeds the result back (marquee)", function()
    local dir = helpers.tmpdir()
    local call = { id = "call_echo", name = "bash", arguments = '{"command":"echo hi"}' }
    -- Slice the first response mid-arguments to exercise SSE reassembly of a
    -- tool call over the socket.
    local wire = helpers.tool_call_block({ call })
    local cut = wire:find("echo hi", 1, true) + 2
    local srv = helpers.start_sse_server({
      helpers.sliced_block(wire, { cut }, "close"),
      { { "send", helpers.text_block("done") }, { "close" } },
    })
    local r = helpers.run_mua({ "-p", "say hi", "--yes" }, helpers.mua_env(srv, dir))
    local s = srv.finish()

    assert.equal(0, r.code)
    assert.equal("done\n", r.stdout)
    assert.equal(2, #s.requests)
    local req2 = s.requests[2].body
    assert.truthy(req2:find('"tool_calls"', 1, true))
    assert.truthy(req2:find('"call_echo"', 1, true))
    assert.truthy(req2:find('"name":"bash"', 1, true))
    assert.truthy(req2:find("echo hi", 1, true)) -- arguments reassembled
    assert.truthy(req2:find('"role":"tool"', 1, true))
    assert.truthy(req2:find('"tool_call_id":"call_echo"', 1, true))
    assert.truthy(req2:find("hi", 1, true)) -- the command's output came back
    -- The tools array is offered identically on both requests.
    assert.truthy(s.requests[1].body:find('"tools"', 1, true))
    assert.truthy(s.requests[2].body:find('"tools"', 1, true))
    helpers.rm_rf(dir)
  end)

  it("refuses a mutating tool in -p without --yes", function()
    local dir = helpers.tmpdir()
    local call = { id = "call_b", name = "bash", arguments = '{"command":"echo hi"}' }
    local srv = helpers.start_sse_server(tool_then_text(call, "understood"))
    local r = helpers.run_mua({ "-p", "do it" }, helpers.mua_env(srv, dir))
    local s = srv.finish()

    assert.equal(0, r.code)
    assert.equal(2, #s.requests)
    assert.truthy(s.requests[2].body:find("requires approval", 1, true))
    helpers.rm_rf(dir)
  end)

  it("runs the ungated read tool in -p without --yes", function()
    local dir = helpers.tmpdir()
    local payload = os.tmpname()
    local f = assert(io.open(payload, "wb"))
    f:write("secret marker 4242\n")
    f:close()
    local call = { id = "call_r", name = "read", arguments = ('{"path":%q}'):format(payload) }
    local srv = helpers.start_sse_server(tool_then_text(call, "got it"))
    local r = helpers.run_mua({ "-p", "read it" }, helpers.mua_env(srv, dir))
    local s = srv.finish()

    assert.equal(0, r.code)
    assert.equal(2, #s.requests)
    assert.truthy(s.requests[2].body:find("secret marker 4242", 1, true))
    os.remove(payload)
    helpers.rm_rf(dir)
  end)

  it("an unknown tool yields an error result, not a crash", function()
    local dir = helpers.tmpdir()
    local call = { id = "call_u", name = "frobnicate", arguments = "{}" }
    local srv = helpers.start_sse_server(tool_then_text(call, "noted"))
    local r = helpers.run_mua({ "-p", "go", "--yes" }, helpers.mua_env(srv, dir))
    local s = srv.finish()

    assert.equal(0, r.code)
    assert.truthy(s.requests[2].body:find("unknown tool", 1, true))
    helpers.rm_rf(dir)
  end)

  it("invalid tool arguments yield an error result", function()
    local dir = helpers.tmpdir()
    local call = { id = "call_x", name = "read", arguments = "{not json" }
    local srv = helpers.start_sse_server(tool_then_text(call, "noted"))
    local r = helpers.run_mua({ "-p", "go", "--yes" }, helpers.mua_env(srv, dir))
    local s = srv.finish()

    assert.equal(0, r.code)
    assert.truthy(s.requests[2].body:find("not valid JSON", 1, true))
    helpers.rm_rf(dir)
  end)

  it("answers two tool calls in one message, in order", function()
    local dir = helpers.tmpdir()
    local p1 = os.tmpname()
    local p2 = os.tmpname()
    for _, pf in ipairs({ { p1, "alpha-one" }, { p2, "beta-two" } }) do
      local f = assert(io.open(pf[1], "wb"))
      f:write(pf[2] .. "\n")
      f:close()
    end
    local block1 = {
      {
        "send",
        helpers.tool_call_block({
          { id = "c1", name = "read", arguments = ('{"path":%q}'):format(p1) },
          { id = "c2", name = "read", arguments = ('{"path":%q}'):format(p2) },
        }),
      },
      { "close" },
    }
    local srv =
      helpers.start_sse_server({ block1, { { "send", helpers.text_block("both") }, { "close" } } })
    local r = helpers.run_mua({ "-p", "read both", "--yes" }, helpers.mua_env(srv, dir))
    local s = srv.finish()

    assert.equal(0, r.code)
    local body = s.requests[2].body
    assert.truthy(body:find('"tool_call_id":"c1"', 1, true))
    assert.truthy(body:find('"tool_call_id":"c2"', 1, true))
    assert.truthy(body:find("alpha-one", 1, true))
    assert.truthy(body:find("beta-two", 1, true))
    assert.truthy(
      body:find('"tool_call_id":"c1"', 1, true) < body:find('"tool_call_id":"c2"', 1, true)
    )
    os.remove(p1)
    os.remove(p2)
    helpers.rm_rf(dir)
  end)

  it("injects a well-formed system message ahead of the conversation", function()
    -- Every other spec sets MUA_SYSTEM_PROMPT="" to suppress the system
    -- message; this one exercises the injection path and pins the request
    -- shape (a reference, not a {"": {...}} nesting that drops the role).
    local dir = helpers.tmpdir()
    local srv = helpers.start_sse_server({ { { "send", helpers.text_block("ok") }, { "close" } } })
    local env = helpers.mua_env(srv, dir)
    env.MUA_SYSTEM_PROMPT = "SYSTEM-PROBE-123"
    local r = helpers.run_mua({ "-p", "hello" }, env)
    local s = srv.finish()

    assert.equal(0, r.code)
    local body = s.requests[1].body
    assert.truthy(body:find('"role":"system"', 1, true))
    assert.truthy(body:find("SYSTEM-PROBE-123", 1, true))
    assert.truthy(body:find('"role":"user"', 1, true))
    assert.is_nil(body:find('{"":', 1, true)) -- the user message must not be empty-key-wrapped
    assert.truthy(body:find('"role":"system"', 1, true) < body:find('"role":"user"', 1, true))
    helpers.rm_rf(dir)
  end)

  it("stops at the step cap without looping", function()
    local dir = helpers.tmpdir()
    local payload = os.tmpname()
    local f = assert(io.open(payload, "wb"))
    f:write("x\n")
    f:close()
    -- Two blocks, both returning a (ungated) read tool call; with MUA_STEP_CAP=2
    -- the loop issues exactly two requests then stops.
    local function read_block(id)
      return {
        {
          "send",
          helpers.tool_call_block({
            { id = id, name = "read", arguments = ('{"path":%q}'):format(payload) },
          }),
        },
        { "close" },
      }
    end
    local srv = helpers.start_sse_server({ read_block("s1"), read_block("s2") })
    local env = helpers.mua_env(srv, dir)
    env.MUA_STEP_CAP = "2"
    local r = helpers.run_mua({ "-p", "loop" }, env)
    local s = srv.finish()

    assert.equal(1, r.code)
    assert.equal(2, #s.requests) -- never a third request
    assert.truthy(r.stderr:match("step cap"))
    os.remove(payload)
    helpers.rm_rf(dir)
  end)

  it("stops at the context-window budget without looping", function()
    local dir = helpers.tmpdir()
    local payload = os.tmpname()
    local f = assert(io.open(payload, "wb"))
    f:write("x\n")
    f:close()
    -- Block 1's usage reports total_tokens 950; with a 1000-token window at 90%
    -- (budget 900) the loop stops after the first request. Block 2 is never asked.
    local function read_block(id, total)
      return {
        {
          "send",
          helpers.tool_call_block({
            { id = id, name = "read", arguments = ('{"path":%q}'):format(payload) },
          }, total and { total = total } or nil),
        },
        { "close" },
      }
    end
    -- Block 2 is scripted to prove no second request is made; the server's
    -- watchdog exits quickly once block 1 is consumed and nothing more comes.
    local srv = helpers.start_sse_server({ read_block("s1", 950), read_block("s2") }, { timeout_ms = 2000 })
    local env = helpers.mua_env(srv, dir)
    env.MUA_CONTEXT_LENGTH = "1000"
    env.MUA_CONTEXT_PCT = "90"
    env.MUA_CONTEXT_WARN_PCT = "0" -- isolate: only the budget message, no warning
    local r = helpers.run_mua({ "-p", "loop" }, env)
    local s = srv.finish()

    assert.equal(1, r.code)
    assert.equal(1, #s.requests) -- stopped before a second request
    assert.truthy(r.stderr:match("context window budget"))
    os.remove(payload)
    helpers.rm_rf(dir)
  end)

  it("warns once when usage crosses the warn threshold, then continues", function()
    local dir = helpers.tmpdir()
    local payload = os.tmpname()
    local f = assert(io.open(payload, "wb"))
    f:write("x\n")
    f:close()
    -- Block 1 reports 800/1000 tokens: past the 75% warn line (750) but under the
    -- 90% budget (900), so the turn warns and proceeds to block 2, which finishes.
    local tool_block = {
      {
        "send",
        helpers.tool_call_block({
          { id = "s1", name = "read", arguments = ('{"path":%q}'):format(payload) },
        }, { total = 800 }),
      },
      { "close" },
    }
    local done_block = { { "send", helpers.text_block("done") }, { "close" } }
    local srv = helpers.start_sse_server({ tool_block, done_block })
    local env = helpers.mua_env(srv, dir)
    env.MUA_CONTEXT_LENGTH = "1000"
    env.MUA_CONTEXT_PCT = "90"
    env.MUA_CONTEXT_WARN_PCT = "75"
    local r = helpers.run_mua({ "-p", "go" }, env)
    local s = srv.finish()

    assert.equal(0, r.code)
    assert.equal(2, #s.requests) -- the warning did not stop the turn
    assert.truthy(r.stderr:match("context window 80%% full"))
    os.remove(payload)
    helpers.rm_rf(dir)
  end)

  it("auto-fetches the model's context window from /models at startup", function()
    local dir = helpers.tmpdir()
    local payload = os.tmpname()
    local f = assert(io.open(payload, "wb"))
    f:write("x\n")
    f:close()
    -- The fixture replays one block per connection in order: connection 1 is the
    -- startup GET /models, connection 2 is the turn. The catalog advertises a
    -- 1000-token window for the model under test; with the turn reporting 950
    -- tokens, the budget (90% -> 900) stops the loop after the one turn request.
    local catalog = '{"data":[{"id":"test/model","context_length":1000,'
      .. '"top_provider":{"context_length":1000}}]}'
    local models_block = {
      {
        "send",
        "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nConnection: close\r\n\r\n" .. catalog,
      },
      { "close" },
    }
    local turn_block = {
      {
        "send",
        helpers.tool_call_block({
          { id = "s1", name = "read", arguments = ('{"path":%q}'):format(payload) },
        }, { total = 950 }),
      },
      { "close" },
    }
    local srv = helpers.start_sse_server({ models_block, turn_block }, { timeout_ms = 4000 })
    local env = helpers.mua_env(srv, dir)
    env.MUA_CONTEXT_LENGTH = nil -- let the startup fetch run against the fixture
    env.MUA_CONTEXT_WARN_PCT = "0"
    local r = helpers.run_mua({ "-p", "go", "-m", "test/model" }, env)
    local s = srv.finish()

    assert.equal(1, r.code)
    assert.equal(2, #s.requests) -- the GET /models, then one turn request
    assert.equal("GET", s.requests[1].method)
    assert.equal("/models", s.requests[1].path)
    assert.truthy(r.stderr:match("context window budget"))
    os.remove(payload)
    helpers.rm_rf(dir)
  end)
end)

describe("the REPL", function()
  it("runs a turn, prints chrome on stderr and text on stdout, then quits", function()
    local dir = helpers.tmpdir()
    local srv =
      helpers.start_sse_server({ { { "send", helpers.text_block("hi back") }, { "close" } } })
    local r = helpers.run_mua({}, helpers.mua_env(srv, dir), { stdin = "hello\nexit\n" })
    local s = srv.finish()

    assert.equal(0, r.code)
    assert.equal(1, #s.requests)
    assert.truthy(r.stdout:find("hi back", 1, true)) -- model text on stdout
    assert.truthy(r.stderr:find("mua>", 1, true)) -- prompt chrome on stderr
    helpers.rm_rf(dir)
  end)

  it("reprompts on an empty line and quits on EOF", function()
    local dir = helpers.tmpdir()
    local srv = helpers.start_sse_server({ { { "send", helpers.text_block("one") }, { "close" } } })
    -- blank line (reprompt), one real turn, then EOF quits.
    local r = helpers.run_mua({}, helpers.mua_env(srv, dir), { stdin = "\nhi\n" })
    local s = srv.finish()

    assert.equal(0, r.code)
    assert.equal(1, #s.requests) -- the blank line started no turn
    helpers.rm_rf(dir)
  end)

  it("approves a gated tool when the user answers y", function()
    local dir = helpers.tmpdir()
    local call = { id = "call_g", name = "bash", arguments = '{"command":"echo hi"}' }
    local srv = helpers.start_sse_server(tool_then_text(call, "ran it"))
    local r = helpers.run_mua({}, helpers.mua_env(srv, dir), { stdin = "do it\ny\n" })
    local s = srv.finish()

    assert.equal(0, r.code)
    assert.equal(2, #s.requests)
    assert.truthy(s.requests[2].body:find('"role":"tool"', 1, true))
    assert.truthy(s.requests[2].body:find("hi", 1, true)) -- the command ran
    helpers.rm_rf(dir)
  end)

  it("declines a gated tool when the user answers n", function()
    local dir = helpers.tmpdir()
    local call = { id = "call_g", name = "bash", arguments = '{"command":"echo hi"}' }
    local srv = helpers.start_sse_server(tool_then_text(call, "fine"))
    local r = helpers.run_mua({}, helpers.mua_env(srv, dir), { stdin = "do it\nn\n" })
    local s = srv.finish()

    assert.equal(0, r.code)
    assert.equal(2, #s.requests)
    assert.truthy(s.requests[2].body:find("declined", 1, true))
    helpers.rm_rf(dir)
  end)

  it("shows the command in the approval prompt, before the y/N", function()
    local dir = helpers.tmpdir()
    local call = { id = "call_g", name = "bash", arguments = '{"command":"echo hi"}' }
    local srv = helpers.start_sse_server(tool_then_text(call, "ran it"))
    local r = helpers.run_mua({}, helpers.mua_env(srv, dir), { stdin = "do it\ny\n" })
    srv.finish()

    -- Regression: the prompt used to show only the tool name, so the y/N came
    -- before the command was ever printed. The command now renders in the prompt.
    assert.truthy(r.stderr:find('allow bash {"command":"echo hi"}', 1, true))
    assert.truthy(r.stderr:find('echo hi"}? [y/N]', 1, true)) -- command precedes the y/N
    helpers.rm_rf(dir)
  end)
end)
