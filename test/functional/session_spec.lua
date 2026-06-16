local helpers = require("test.functional.helpers")

-- End-to-end coverage of the session scope (mua_sess_*): an init.lua fixture
-- registers hooks that read mua.sess.get_messages(0)/get_id(0) and append what
-- they saw to MUA_SESS_LOG. The SSE fixture drives a read tool call then a final
-- answer, so the conversation grows user -> assistant(tool_calls) -> tool ->
-- assistant, and the hooks observe it at SessionStart (empty), ToolPre, and
-- SessionEnd.

local function tool_then_text(call, text)
  return {
    { { "send", helpers.tool_call_block({ call }) }, { "close" } },
    { { "send", helpers.text_block(text) }, { "close" } },
  }
end

local function run(config_dir, call, prompt_args)
  local dir = helpers.tmpdir()
  local logfile = os.tmpname()
  os.remove(logfile) -- the hooks create it on first append
  local srv = helpers.start_sse_server(tool_then_text(call, "done"))
  local env = helpers.mua_env(srv, dir)
  env.MUA_CONFIG_DIR = config_dir
  env.MUA_SESS_LOG = logfile
  local r = helpers.run_mua(prompt_args, env)
  local s = srv.finish()
  local lines = {}
  local f = io.open(logfile, "r")
  if f then
    for line in f:lines() do
      lines[#lines + 1] = line
    end
    f:close()
  end
  os.remove(logfile)
  helpers.rm_rf(dir)
  return r, s, lines
end

-- The value logged for `key` (lines are "key=value"); nil if absent.
local function value_of(lines, key)
  for _, line in ipairs(lines) do
    local v = line:match("^" .. key .. "=(.*)$")
    if v then
      return v
    end
  end
  return nil
end

local FIX = "test/functional/fixtures/session"

describe("session scope (mua_sess_*)", function()
  it("a hook reads the conversation of record via mua.sess.get_messages/get_id(0)", function()
    local payload = os.tmpname()
    local f = assert(io.open(payload, "wb"))
    f:write("hello-file\n")
    f:close()
    local call = { id = "c1", name = "read", arguments = ('{"path":%q}'):format(payload) }
    local r, s, lines = run(FIX, call, { "-p", "go" })
    os.remove(payload)

    assert.equal(0, r.code)

    -- get_id(0) resolves to the session SessionStart named, and the same id
    -- holds throughout the run (a stable handle).
    local id = value_of(lines, "ev_session")
    assert.truthy(id and id:match("^%d%d%d%d%d%d%d%dT%d%d%d%d%d%d_%d%d$"))
    assert.equal(id, value_of(lines, "start_id"))
    assert.equal(id, value_of(lines, "pre_id"))

    -- SessionStart: the session is empty (the user prompt is appended later).
    assert.equal("0", value_of(lines, "start_count"))

    -- ToolPre (mid-turn): the user prompt + the assistant's tool-call message.
    assert.equal("2", value_of(lines, "pre_count"))
    assert.equal("user", value_of(lines, "pre_role1"))
    assert.equal("assistant", value_of(lines, "pre_role2"))
    assert.equal("go", value_of(lines, "pre_first_content")) -- the Dict marshaled through

    -- SessionEnd: + the tool result + the assistant's final answer.
    assert.equal("4", value_of(lines, "end_count"))
    assert.equal("tool", value_of(lines, "end_role3"))

    -- sanity: the read actually ran and its result reached the model.
    assert.truthy(s.requests[2].body:find("hello-file", 1, true))
  end)
end)
