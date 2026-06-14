local helpers = require("test.functional.helpers")

-- Session persistence and --resume. MUA_SYSTEM_PROMPT="" (via mua_env) keeps
-- the request messages array equal to the conversation, so it can be asserted
-- exactly.

-- Read the single session file under <state>/sessions; returns its lines.
local function session_lines(state_dir)
  local p = io.popen("cat " .. state_dir .. "/sessions/*.jsonl 2>/dev/null")
  local data = p:read("*a") or ""
  p:close()
  local lines = {}
  for line in data:gmatch("[^\n]+") do
    lines[#lines + 1] = line
  end
  return lines
end

describe("session persistence", function()
  it("-p writes a 3-line session: header, user, assistant", function()
    local dir = helpers.tmpdir()
    local srv =
      helpers.start_sse_server({ { { "send", helpers.text_block("hello there") }, { "close" } } })
    local r = helpers.run_mua({ "-p", "greet me" }, helpers.mua_env(srv, dir))
    local s = srv.finish()

    assert.equal(0, r.code)
    assert.equal(0, s.code)
    local lines = session_lines(dir)
    assert.equal(3, #lines)
    assert.truthy(lines[1]:find('"type":"session"', 1, true))
    assert.truthy(lines[2]:find('"role":"user"', 1, true))
    assert.truthy(lines[2]:find("greet me", 1, true))
    assert.truthy(lines[3]:find('"role":"assistant"', 1, true))
    assert.truthy(lines[3]:find("hello there", 1, true))
    helpers.rm_rf(dir)
  end)

  it("--resume replays the prior conversation into the next request", function()
    local dir = helpers.tmpdir()
    local srv1 =
      helpers.start_sse_server({ { { "send", helpers.text_block("four") }, { "close" } } })
    local r1 = helpers.run_mua({ "-p", "what is 2+2" }, helpers.mua_env(srv1, dir))
    srv1.finish()
    assert.equal(0, r1.code)

    local srv2 =
      helpers.start_sse_server({ { { "send", helpers.text_block("eight") }, { "close" } } })
    local r2 = helpers.run_mua({ "--resume", "-p", "double it" }, helpers.mua_env(srv2, dir))
    local s2 = srv2.finish()

    assert.equal(0, r2.code)
    assert.equal(1, #s2.requests)
    -- The resumed request carries the whole prior conversation plus the new
    -- user line: [user, assistant, user] (system suppressed).
    local body = s2.requests[1].body
    assert.truthy(body:find("what is 2+2", 1, true))
    assert.truthy(body:find("four", 1, true))
    assert.truthy(body:find("double it", 1, true))
    -- assistant("four") precedes the new user("double it")
    assert.truthy(body:find("four", 1, true) < body:find("double it", 1, true))
    assert.equal(5, #session_lines(dir)) -- header + user + assistant + user + assistant
    helpers.rm_rf(dir)
  end)

  it("--resume with no sessions starts fresh and succeeds", function()
    local dir = helpers.tmpdir()
    local srv =
      helpers.start_sse_server({ { { "send", helpers.text_block("fresh") }, { "close" } } })
    local r = helpers.run_mua({ "--resume", "-p", "hi" }, helpers.mua_env(srv, dir))
    local s = srv.finish()

    assert.equal(0, r.code)
    assert.equal(0, s.code)
    assert.truthy(r.stderr:match("no session to resume"))
    assert.equal(3, #session_lines(dir))
    helpers.rm_rf(dir)
  end)

  it("--resume of a corrupt latest fails hard, naming the line", function()
    local dir = helpers.tmpdir()
    assert(os.execute("mkdir -p " .. dir .. "/sessions"))
    -- A header, a corrupt line 2, and a line 3 after it: mid-file corruption.
    local f = assert(io.open(dir .. "/sessions/20260101T000000_00.jsonl", "wb"))
    f:write('{"type":"session","version":1,"id":"20260101T000000_00","created":0}\n')
    f:write("{ this is not json\n")
    f:write('{"role":"user","content":"hi"}\n')
    f:close()
    -- mua fails at session load, before issuing any request, so no live server
    -- is needed; the base URL is never dialed.
    local r = helpers.run_mua({ "--resume", "-p", "hi" }, {
      OPENROUTER_API_KEY = "test-key-123",
      OPENROUTER_BASE_URL = "http://127.0.0.1:1",
      MUA_CONFIG_DIR = "test/functional/fixtures/nonexistent",
      MUA_STATE_DIR = dir,
      MUA_SYSTEM_PROMPT = "",
      MUA_LOG = "",
    })
    assert.equal(1, r.code)
    assert.truthy(r.stderr:match("corrupt line"))
    helpers.rm_rf(dir)
  end)

  it("--resume repairs a dangling tool_calls tail before the next request", function()
    local dir = helpers.tmpdir()
    assert(os.execute("mkdir -p " .. dir .. "/sessions"))
    -- Tail is an assistant message with an unanswered tool_call (crash artifact).
    local f = assert(io.open(dir .. "/sessions/20260101T000000_00.jsonl", "wb"))
    f:write('{"type":"session","version":1,"id":"20260101T000000_00","created":0}\n')
    f:write('{"role":"user","content":"list"}\n')
    f:write(
      '{"role":"assistant","content":null,"tool_calls":[{"id":"call_Z",'
        .. '"type":"function","function":{"name":"bash","arguments":"{}"}}]}\n'
    )
    f:close()
    local srv = helpers.start_sse_server({ { { "send", helpers.text_block("ok") }, { "close" } } })
    local r = helpers.run_mua({ "--resume", "-p", "continue" }, helpers.mua_env(srv, dir))
    local s = srv.finish()

    assert.equal(0, r.code)
    assert.equal(1, #s.requests)
    -- The synthetic tool result answers call_Z before the new user message.
    local body = s.requests[1].body
    assert.truthy(body:find('"tool_call_id":"call_Z"', 1, true))
    assert.truthy(body:find("interrupted", 1, true))
    assert.truthy(body:find('"tool_call_id":"call_Z"', 1, true) < body:find("continue", 1, true))
    helpers.rm_rf(dir)
  end)
end)
