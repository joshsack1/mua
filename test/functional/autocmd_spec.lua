local helpers = require("test.functional.helpers")

-- End-to-end coverage of the autocmd event set: an init.lua fixture registers
-- hooks that append markers to MUA_AUTOCMD_LOG, the SSE fixture drives a tool
-- call then a final answer, and we assert which hooks fired (and that a ToolPre
-- veto refused the tool). The first response is the tool call, the second the
-- text, so every case sends two requests and one session start/end pair.

local function tool_then_text(call, text)
  return {
    { { "send", helpers.tool_call_block({ call }) }, { "close" } },
    { { "send", helpers.text_block(text) }, { "close" } },
  }
end

-- Runs a turn whose first response calls `call`, with the hooks in `config_dir`
-- logging to a temp file. Returns the run result, the captured requests, and
-- the log file's lines.
local function run(config_dir, call, prompt_args)
  local dir = helpers.tmpdir()
  local logfile = os.tmpname()
  os.remove(logfile) -- the hooks create it on first append
  local srv = helpers.start_sse_server(tool_then_text(call, "done"))
  local env = helpers.mua_env(srv, dir)
  env.MUA_CONFIG_DIR = config_dir
  env.MUA_AUTOCMD_LOG = logfile
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

local function has(lines, needle)
  for _, line in ipairs(lines) do
    if line:find(needle, 1, true) then
      return true
    end
  end
  return false
end

local function count_prefix(lines, prefix)
  local n = 0
  for _, line in ipairs(lines) do
    if line:sub(1, #prefix) == prefix then
      n = n + 1
    end
  end
  return n
end

local FIX = "test/functional/fixtures/autocmd"

describe("autocmd events", function()
  it("fires ToolPre, ToolPost, StreamDelta, and Session events around a tool call", function()
    local payload = os.tmpname()
    local f = assert(io.open(payload, "wb"))
    f:write("hello-file\n")
    f:close()
    local call = { id = "c1", name = "read", arguments = ('{"path":%q}'):format(payload) }
    local r, s, lines = run(FIX, call, { "-p", "go" })
    os.remove(payload)

    assert.equal(0, r.code)
    -- ToolPre fires for the ungated read tool: proof it fires for *every* tool.
    assert.is_true(has(lines, "pre read"))
    -- ToolPost fires with the (successful) result.
    assert.is_true(has(lines, "post read err=false"))
    -- Session events fire exactly once each.
    assert.equal(1, count_prefix(lines, "start "))
    assert.equal(1, count_prefix(lines, "end "))
    -- StreamDelta saw the final answer.
    assert.is_true(has(lines, "delta done"))
    -- the read result reached the model.
    assert.truthy(s.requests[2].body:find("hello-file", 1, true))
  end)

  it("a ToolPre hook vetoes a tool; the model sees the reason, the tool never runs", function()
    -- The path mentions "secret", so the hook vetoes; read is never executed
    -- (no real file needed).
    local call = { id = "c2", name = "read", arguments = '{"path":"/tmp/has-secret-here.txt"}' }
    local r, s, lines = run(FIX, call, { "-p", "go" })

    assert.equal(0, r.code)
    assert.is_true(has(lines, "pre read")) -- the hook fired
    local req2 = s.requests[2].body
    assert.truthy(req2:find("blocked: secret file", 1, true)) -- veto reason reached the model
    -- A vetoed tool still "completes" with the synthetic error result.
    assert.is_true(has(lines, "post read err=true"))
  end)

  it("a throwing hook is nonfatal: the tool still runs and the turn completes", function()
    local payload = os.tmpname()
    local f = assert(io.open(payload, "wb"))
    f:write("survived\n")
    f:close()
    local call = { id = "c3", name = "read", arguments = ('{"path":%q}'):format(payload) }
    local r, s, lines = run("test/functional/fixtures/autocmd_throw", call, { "-p", "go" })
    os.remove(payload)

    assert.equal(0, r.code) -- the throw was caught, not propagated
    assert.is_true(has(lines, "threw for read")) -- the hook ran (then raised)
    assert.truthy(s.requests[2].body:find("survived", 1, true)) -- a throw is not a veto
  end)

  it("a ToolPre hook rewrites a read's path; the rewritten path is what executes", function()
    -- The model asks to read a non-existent "_DECOY" path; the hook rewrites it to
    -- the "_REAL" sibling we wrote. Proof: the real file's content reaches the model
    -- -- the decoy never existed, so a missed rewrite would surface a read error.
    local base = os.tmpname()
    os.remove(base)
    local real = base .. "_REAL"
    local f = assert(io.open(real, "wb"))
    f:write("REWRITTEN-CONTENT\n")
    f:close()
    local call = { id = "cr1", name = "read", arguments = ('{"path":%q}'):format(base .. "_DECOY") }
    local r, s = run("test/functional/fixtures/autocmd_rewrite", call, { "-p", "go" })
    os.remove(real)

    assert.equal(0, r.code)
    assert.truthy(s.requests[2].body:find("REWRITTEN-CONTENT", 1, true)) -- the rewrite executed
  end)

  it(
    "a ToolPre rewrite of a mutating bash command still runs (under --yes) as rewritten",
    function()
      -- bash is mutating, so --yes lets it run; the hook rewrites the command and the
      -- rewritten output is what comes back. Mirrors the grep->rg use case: the rewrite
      -- changes what runs, while approval still applies (here, granted by --yes).
      local call = { id = "cb1", name = "bash", arguments = '{"command":"echo original"}' }
      local r, s = run("test/functional/fixtures/autocmd_rewrite", call, { "-y", "-p", "go" })

      assert.equal(0, r.code)
      assert.truthy(s.requests[2].body:find("rewritten", 1, true)) -- echo rewritten ran, not original
    end
  )

  it("a ToolPre hook approving (return true) runs a mutating tool with no prompt", function()
    -- In the REPL (interactive base gate, no --yes), the approve fixture returns
    -- true for bash, so the call runs outright: no "allow bash" y/N prompt fires
    -- and the command's output reaches the model. Proof the approve outcome skips
    -- the base gate -- the no-prompt-for-allowlisted-commands case.
    local dir = helpers.tmpdir()
    local call = { id = "cap1", name = "bash", arguments = '{"command":"echo APPROVED_RAN"}' }
    local srv = helpers.start_sse_server(tool_then_text(call, "done"))
    local env = helpers.mua_env(srv, dir)
    env.MUA_CONFIG_DIR = "test/functional/fixtures/autocmd_approve"
    local r = helpers.run_mua({}, env, { stdin = "go\n" }) -- REPL; EOF ends it
    local s = srv.finish()
    helpers.rm_rf(dir)

    assert.equal(0, r.code)
    assert.is_nil(r.stderr:find("allow bash", 1, true)) -- the gate never prompted
    assert.truthy(s.requests[2].body:find("APPROVED_RAN", 1, true)) -- the command ran
  end)

  it("a ToolPre approve runs a mutating tool under -p, which would otherwise refuse", function()
    -- Under -p the base gate auto-refuses mutating tools; the approve hook makes
    -- bash run anyway, so the real output reaches the model instead of the
    -- synthetic "requires approval" refusal.
    local call = { id = "cap2", name = "bash", arguments = '{"command":"echo APPROVED_RAN"}' }
    local r, s = run("test/functional/fixtures/autocmd_approve", call, { "-p", "go" })

    assert.equal(0, r.code)
    assert.truthy(s.requests[2].body:find("APPROVED_RAN", 1, true))
    assert.is_nil(s.requests[2].body:find("requires approval", 1, true))
  end)
end)
