local helpers = require("test.functional.helpers")

-- End-to-end coverage of the UserPromptPre event: an init.lua fixture registers
-- a hook that swallows "/skip", rewrites "rewriteme", and passes anything else
-- through. The event fires before the turn, so we assert on the captured request
-- (or its absence) rather than on hook side effects.

local FIX = "test/functional/fixtures/userprompt"

local function text(t)
  return { { { "send", helpers.text_block(t) }, { "close" } } }
end

-- Runs `mua -p <prompt>` against a one-response SSE server with the fixture's
-- hook installed. Returns the run result and the server's finish() summary.
local function run(prompt, blocks, server_opts)
  local dir = helpers.tmpdir()
  local srv = helpers.start_sse_server(blocks, server_opts)
  local env = helpers.mua_env(srv, dir)
  env.MUA_CONFIG_DIR = FIX
  local r = helpers.run_mua({ "-p", prompt }, env)
  local s = srv.finish()
  helpers.rm_rf(dir)
  return r, s
end

describe("UserPromptPre event", function()
  it("a hook returning false swallows the prompt: no turn runs", function()
    -- The model is never contacted; the short timeout lets the idle server exit
    -- quickly since no request will arrive.
    local r, s = run("/skip", text("unused"), { timeout_ms = 2000 })

    assert.equal(0, r.code)
    assert.equal(0, #s.requests) -- swallowed: the provider was never called
  end)

  it("a hook returning a string rewrites the prompt before the turn", function()
    local r, s = run("rewriteme", text("done"))

    assert.equal(0, r.code)
    assert.equal(1, #s.requests)
    local body = s.requests[1].body
    assert.truthy(body:find("REWRITTEN-PROMPT", 1, true)) -- the rewrite reached the model
    assert.falsy(body:find("rewriteme", 1, true)) -- the original did not
  end)

  it("an unmatched prompt passes through unchanged", function()
    local r, s = run("hello there", text("done"))

    assert.equal(0, r.code)
    assert.equal(1, #s.requests)
    assert.truthy(s.requests[1].body:find("hello there", 1, true))
  end)
end)
