local helpers = require("test.functional.helpers")

-- End-to-end coverage of mua_register_tool: an init.lua fixture registers tools
-- (see fixtures/tools_register), the SSE fixture has the model call one, and we
-- inspect the captured requests. This is the only place the cJSON<->Object<->Lua
-- tool path runs whole -- args marshaled in, the Lua callback run, its result
-- marshaled back into the next request. First response = the tool call, second =
-- the final text; so every case sends exactly two requests.
local FIX = "test/functional/fixtures/tools_register"

local function tool_then_text(call, text)
  return {
    { { "send", helpers.tool_call_block({ call }) }, { "close" } },
    { { "send", helpers.text_block(text) }, { "close" } },
  }
end

local function run(call, prompt_args)
  local dir = helpers.tmpdir()
  local srv = helpers.start_sse_server(tool_then_text(call, "done"))
  local env = helpers.mua_env(srv, dir)
  env.MUA_CONFIG_DIR = FIX
  local r = helpers.run_mua(prompt_args, env)
  local s = srv.finish()
  helpers.rm_rf(dir)
  return r, s
end

describe("mua_register_tool", function()
  it("advertises a registered tool and feeds its result back", function()
    local call = { id = "c1", name = "echo_tool", arguments = '{"text":"hello"}' }
    local r, s = run(call, { "-p", "use echo_tool" })
    assert.equal(0, r.code)
    assert.equal(2, #s.requests)
    -- The tool (and its schema) is advertised in the first request's tools array.
    assert.truthy(s.requests[1].body:find('"name":"echo_tool"', 1, true))
    assert.truthy(s.requests[1].body:find('"required":["text"]', 1, true))
    -- The callback ran; its result returned as the tool message.
    local req2 = s.requests[2].body
    assert.truthy(req2:find('"role":"tool"', 1, true))
    assert.truthy(req2:find('"tool_call_id":"c1"', 1, true))
    assert.truthy(req2:find("echoed:hello", 1, true))
    assert.equal("done\n", r.stdout)
  end)

  it("JSON-encodes a non-string (table) result", function()
    local call = { id = "c2", name = "table_tool", arguments = "{}" }
    local r, s = run(call, { "-p", "use table_tool" })
    assert.equal(0, r.code)
    -- The returned table was JSON-encoded into the tool content: both the key
    -- and the value survive (a verbatim string result would carry neither).
    local req2 = s.requests[2].body
    assert.truthy(req2:find("greeting", 1, true))
    assert.truthy(req2:find("hi-from-table", 1, true))
  end)

  it("turns a raising callback into a result and continues the turn", function()
    local call = { id = "c3", name = "boom_tool", arguments = "{}" }
    local r, s = run(call, { "-p", "use boom_tool" })
    assert.equal(0, r.code) -- the raise became a result, not a turn failure
    assert.equal(2, #s.requests)
    assert.truthy(s.requests[2].body:find("boom-from-callback", 1, true))
    assert.equal("done\n", r.stdout) -- the follow-up answer still streamed
  end)

  it("refuses a gated (mutating) tool under -p without --yes", function()
    local call = { id = "c4", name = "gated_tool", arguments = "{}" }
    local r, s = run(call, { "-p", "use gated_tool" })
    assert.equal(0, r.code)
    assert.truthy(s.requests[2].body:find("requires approval", 1, true))
    assert.is_nil(s.requests[2].body:find("gated-ran", 1, true)) -- callback never ran
  end)

  it("runs a gated tool when approved with --yes", function()
    local call = { id = "c5", name = "gated_tool", arguments = "{}" }
    local r, s = run(call, { "-p", "use gated_tool", "--yes" })
    assert.equal(0, r.code)
    assert.truthy(s.requests[2].body:find("gated-ran", 1, true))
  end)
end)
