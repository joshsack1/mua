local t = require("test.unit.helpers")
local ffi = t.ffi
local lib = t.lib

-- agent_repair_session operates on a loaded session; the rest of the agent
-- loop needs http + the event loop and is covered functionally. cJSON / Error
-- / String / json_* come from helpers; declare only what is local here.
t.cdef([[
  typedef struct SessionState SessionState;
  SessionState *session_new(Error *err);
  bool session_append(SessionState *sess, cJSON *msg, Error *err);
  size_t session_message_count(const SessionState *sess);
  const cJSON *session_message_get(const SessionState *sess, size_t idx);
  void session_free(SessionState *sess);

  bool agent_repair_session(SessionState *sess, Error *err);

  int setenv(const char *name, const char *value, int overwrite);
]])

lib.json_init()

local anchors = {}
local function S(str)
  anchors[#anchors + 1] = str
  return ffi.new("String", ffi.cast("char *", str), #str)
end

-- A fresh isolated state dir per test, so session_new never touches the
-- developer's ~/.local/state/mua.
local function isolate_state()
  local dir = os.tmpname()
  os.remove(dir) -- session_new's paths_ensure_dir creates it
  assert.equal(0, ffi.C.setenv("MUA_STATE_DIR", dir, 1))
  return dir
end

local function new_session()
  local err = ffi.new("Error")
  err.type = ffi.C.kErrorTypeNone
  local sess = lib.session_new(err)
  assert.is_true(sess ~= nil)
  return sess
end

local function append_json(sess, json)
  local err = ffi.new("Error")
  err.type = ffi.C.kErrorTypeNone
  local msg = lib.json_parse(S(json), 1048576, err)
  assert.is_true(msg ~= nil)
  assert.is_true(lib.session_append(sess, msg, err))
end

local function repair(sess)
  local err = ffi.new("Error")
  err.type = ffi.C.kErrorTypeNone
  local ok = lib.agent_repair_session(sess, err)
  local msg = err.msg ~= nil and ffi.string(err.msg) or nil
  lib.api_clear_error(err)
  return ok, msg
end

describe("agent_repair_session", function()
  after_each(function()
    anchors = {}
  end)

  it("answers every unanswered call in a dangling assistant tail", function()
    isolate_state()
    local sess = new_session()
    append_json(sess, '{"role":"user","content":"go"}')
    append_json(
      sess,
      '{"role":"assistant","content":null,"tool_calls":['
        .. '{"id":"call_A","type":"function","function":{"name":"bash","arguments":"{}"}},'
        .. '{"id":"call_B","type":"function","function":{"name":"read","arguments":"{}"}}]}'
    )
    local before = tonumber(lib.session_message_count(sess))
    assert.is_true((repair(sess)))
    local after = tonumber(lib.session_message_count(sess))
    assert.equal(before + 2, after) -- two synthetic tool results appended
    -- Verify by reloading the printed messages: the two new tail messages are
    -- role:"tool" answering call_A then call_B in order.
    local m1 = lib.json_print(lib.session_message_get(sess, after - 2))
    local m2 = lib.json_print(lib.session_message_get(sess, after - 1))
    assert.truthy(ffi.string(m1.data, m1.size):find('"tool_call_id":"call_A"', 1, true))
    assert.truthy(ffi.string(m1.data, m1.size):find('"role":"tool"', 1, true))
    assert.truthy(ffi.string(m1.data, m1.size):find("interrupted", 1, true))
    assert.truthy(ffi.string(m2.data, m2.size):find('"tool_call_id":"call_B"', 1, true))
    lib.xfree(m1.data)
    lib.xfree(m2.data)
    lib.session_free(sess)
  end)

  it("only fills the calls left unanswered", function()
    isolate_state()
    local sess = new_session()
    append_json(
      sess,
      '{"role":"assistant","content":null,"tool_calls":['
        .. '{"id":"call_A","type":"function","function":{"name":"read","arguments":"{}"}},'
        .. '{"id":"call_B","type":"function","function":{"name":"read","arguments":"{}"}}]}'
    )
    append_json(sess, '{"role":"tool","tool_call_id":"call_A","content":"done"}')
    local before = tonumber(lib.session_message_count(sess))
    assert.is_true((repair(sess)))
    local after = tonumber(lib.session_message_count(sess))
    assert.equal(before + 1, after) -- only call_B was missing
    local m = lib.json_print(lib.session_message_get(sess, after - 1))
    assert.truthy(ffi.string(m.data, m.size):find('"tool_call_id":"call_B"', 1, true))
    lib.xfree(m.data)
    lib.session_free(sess)
  end)

  it("is a no-op for a fully answered tail", function()
    isolate_state()
    local sess = new_session()
    append_json(
      sess,
      '{"role":"assistant","content":null,"tool_calls":['
        .. '{"id":"call_A","type":"function","function":{"name":"read","arguments":"{}"}}]}'
    )
    append_json(sess, '{"role":"tool","tool_call_id":"call_A","content":"done"}')
    local before = tonumber(lib.session_message_count(sess))
    assert.is_true((repair(sess)))
    assert.equal(before, tonumber(lib.session_message_count(sess)))
    lib.session_free(sess)
  end)

  it("is a no-op when there is no tool-call round", function()
    isolate_state()
    local sess = new_session()
    append_json(sess, '{"role":"user","content":"hi"}')
    append_json(sess, '{"role":"assistant","content":"hello"}')
    local before = tonumber(lib.session_message_count(sess))
    assert.is_true((repair(sess)))
    assert.equal(before, tonumber(lib.session_message_count(sess)))
    lib.session_free(sess)
  end)
end)
