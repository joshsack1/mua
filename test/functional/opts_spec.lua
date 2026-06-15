local helpers = require("test.functional.helpers")

-- Options set in init.lua flow through the mua.api bridge into the C store.
-- These exercise the bridge end to end without a server: the REPL evaluates
-- init.lua at startup, then EOF on stdin quits with exit 0. init.lua print()
-- lands on stdout; a raised error lands on stderr (nonfatal, nvim-style).
-- The behavioral effect of each option on a request is covered separately
-- once the agent reads the store.
local function opts_env(config_dir)
  return {
    MUA_CONFIG_DIR = config_dir,
    OPENROUTER_API_KEY = "test-key", -- required to reach the REPL; never used (EOF quits)
    MUA_STATE_DIR = helpers.tmpdir(),
    MUA_LOG = "",
  }
end

describe("mua.o options bridge", function()
  it("sets and reads back options of each type through mua.o", function()
    local r = helpers.run_mua({}, opts_env("test/functional/fixtures/opts_load"), { stdin = "" })
    assert.equal(0, r.code)
    assert.truthy(r.stdout:find("STEPCAP=7", 1, true)) -- integer round-trip
    assert.truthy(r.stdout:find("MODEL=demo/model", 1, true)) -- string round-trip
    assert.truthy(r.stdout:find("SP1=You", 1, true)) -- system_prompt round-trip
  end)

  it("reports an unknown option but stays nonfatal", function()
    local r = helpers.run_mua({}, opts_env("test/functional/fixtures/opts_unknown"), { stdin = "" })
    assert.equal(0, r.code)
    assert.truthy(r.stderr:find("init.lua", 1, true))
    assert.truthy(r.stderr:find("bogus", 1, true))
  end)
end)

describe("mua.o options shape the request", function()
  -- Drive a real turn against the fixture and inspect the captured request.
  -- mua_env hardcodes MUA_SYSTEM_PROMPT="" and an empty config dir, so each
  -- test overrides MUA_CONFIG_DIR and, where it exercises the lua
  -- system_prompt layer, removes MUA_SYSTEM_PROMPT (env wins over lua, so it
  -- must be absent for the lua value to surface; the harness env has none).
  local function turn(config_dir, args, env_overrides)
    local dir = helpers.tmpdir()
    local srv = helpers.start_sse_server({ { { "send", helpers.text_block("ok") }, { "close" } } })
    local env = helpers.mua_env(srv, dir)
    env.MUA_CONFIG_DIR = config_dir
    for k, v in pairs(env_overrides or {}) do
      env[k] = v ~= false and v or nil -- a value of false removes the key
    end
    local r = helpers.run_mua(args, env)
    local s = srv.finish()
    helpers.rm_rf(dir)
    return r, s
  end

  it("uses mua.o.model in the request body", function()
    local r, s = turn("test/functional/fixtures/opts_model", { "-p", "hi" })
    assert.equal(0, r.code)
    assert.truthy(s.requests[1].body:find('"model":"z-ai/glm-5.1"', 1, true))
  end)

  it("lets a CLI -m override mua.o.model", function()
    local r, s = turn("test/functional/fixtures/opts_model", { "-p", "hi", "-m", "cli/override" })
    assert.equal(0, r.code)
    assert.truthy(s.requests[1].body:find('"model":"cli/override"', 1, true))
    assert.is_nil(s.requests[1].body:find("z-ai/glm-5.1", 1, true))
  end)

  it("omits the system message when mua.o.system_prompt is empty", function()
    local r, s = turn(
      "test/functional/fixtures/opts_sp_omit",
      { "-p", "hi" },
      { MUA_SYSTEM_PROMPT = false }
    )
    assert.equal(0, r.code)
    assert.is_nil(s.requests[1].body:find('"role":"system"', 1, true))
  end)

  it("injects mua.o.system_prompt as the system message", function()
    local r, s = turn(
      "test/functional/fixtures/opts_sp_set",
      { "-p", "hi" },
      { MUA_SYSTEM_PROMPT = false }
    )
    assert.equal(0, r.code)
    assert.truthy(s.requests[1].body:find('"role":"system"', 1, true))
    assert.truthy(s.requests[1].body:find("GUIDANCE-XYZ", 1, true))
  end)

  it("lets env MUA_SYSTEM_PROMPT override mua.o.system_prompt", function()
    local r, s = turn(
      "test/functional/fixtures/opts_sp_set",
      { "-p", "hi" },
      { MUA_SYSTEM_PROMPT = "FROM-ENV-123" }
    )
    assert.equal(0, r.code)
    assert.truthy(s.requests[1].body:find("FROM-ENV-123", 1, true))
    assert.is_nil(s.requests[1].body:find("GUIDANCE-XYZ", 1, true))
  end)

  it("stops at mua.o.step_cap", function()
    local dir = helpers.tmpdir()
    local payload = os.tmpname()
    local f = assert(io.open(payload, "wb"))
    f:write("x\n")
    f:close()
    -- One ungated read tool call; with step_cap=1 the loop issues exactly one
    -- request, runs the tool, then stops at the cap (never a second request).
    local block = {
      {
        "send",
        helpers.tool_call_block({
          { id = "s1", name = "read", arguments = ('{"path":%q}'):format(payload) },
        }),
      },
      { "close" },
    }
    local srv = helpers.start_sse_server({ block })
    local env = helpers.mua_env(srv, dir)
    env.MUA_CONFIG_DIR = "test/functional/fixtures/opts_stepcap"
    local r = helpers.run_mua({ "-p", "loop" }, env)
    local s = srv.finish()
    assert.equal(1, r.code)
    assert.equal(1, #s.requests)
    assert.truthy(r.stderr:match("step cap"))
    os.remove(payload)
    helpers.rm_rf(dir)
  end)
end)
