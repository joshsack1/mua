local t = require("test.unit.helpers")
local ffi = t.ffi
local lib = t.lib

-- Declarations mirror src/mua/session.h (cJSON and json_parse/print/free
-- come from helpers). Session symbols are unique to this spec.
t.cdef([[
  typedef struct SessionState SessionState;
  SessionState *session_new(Error *err);
  SessionState *session_load(const char *path, Error *err);
  SessionState *session_load_latest(Error *err);
  bool session_append(SessionState *sess, cJSON *msg, Error *err);
  size_t session_message_count(const SessionState *sess);
  const cJSON *session_message_get(const SessionState *sess, size_t idx);
  const cJSON *session_messages(const SessionState *sess);
  const char *session_id(const SessionState *sess);
  void session_set_current(SessionState *sess);
  SessionState *session_resolve(int32_t handle, Error *err);
  void session_free(SessionState *sess);
]])

-- Duplicates paths_spec's libc set, kept in its own block: if that spec ran
-- first, the whole-block redefine abort leaves nothing missing.
t.cdef([[
  int setenv(const char *name, const char *value, int overwrite);
  int unsetenv(const char *name);
]])

lib.json_init()

local BIG = 16 * 1024 * 1024 -- parse cap for spec-built documents

local anchors = {}

--- Build a String view over a Lua string (anchored against GC for the test).
local function S(str)
  anchors[#anchors + 1] = str
  return ffi.new("String", ffi.cast("char *", str), #str)
end

local function new_error()
  local err = ffi.new("Error")
  err.type = ffi.C.kErrorTypeNone
  return err
end

--- json_parse a literal, asserting success.
local function parse(text)
  local perr = new_error()
  local node = lib.json_parse(S(text), BIG, perr)
  assert.equal(ffi.C.kErrorTypeNone, perr.type)
  assert.is_true(node ~= nil)
  return node
end

--- json_print to a Lua string (frees the C buffer).
local function printed(node)
  local s = lib.json_print(node)
  local out = ffi.string(s.data, s.size)
  lib.xfree(s.data)
  return out
end

local function read_file(path)
  local f = assert(io.open(path, "rb"))
  local data = f:read("*a")
  f:close()
  return data
end

describe("session", function()
  local root, sdir, err
  local saved_state_dir

  local function header_line(id)
    return '{"type":"session","version":1,"id":"' .. id .. '","created":1}'
  end

  --- Drop a raw fixture file into the sessions dir.
  local function put_session(name, body)
    assert(os.execute('mkdir -p "' .. sdir .. '"'))
    local f = assert(io.open(sdir .. "/" .. name, "wb"))
    f:write(body)
    f:close()
  end

  before_each(function()
    saved_state_dir = os.getenv("MUA_STATE_DIR")
    root = os.tmpname()
    os.remove(root) -- want the free name, not the file tmpname may create
    sdir = root .. "/sessions"
    assert.equal(0, ffi.C.setenv("MUA_STATE_DIR", root, 1))
    err = new_error()
  end)

  after_each(function()
    lib.api_clear_error(err)
    if saved_state_dir then
      assert.equal(0, ffi.C.setenv("MUA_STATE_DIR", saved_state_dir, 1))
    else
      assert.equal(0, ffi.C.unsetenv("MUA_STATE_DIR"))
    end
    os.execute('rm -rf "' .. root .. '"')
  end)

  it("session_new writes a header line in a 0600 file", function()
    local sess = lib.session_new(err)
    assert.is_true(sess ~= nil)
    assert.equal(ffi.C.kErrorTypeNone, err.type)
    local id = ffi.string(lib.session_id(sess))
    assert.truthy(id:match("^%d%d%d%d%d%d%d%dT%d%d%d%d%d%d_%d%d$"))
    local path = sdir .. "/" .. id .. ".jsonl"
    local pipe = assert(io.popen('ls -l "' .. path .. '"'))
    local mode = pipe:read("*l") or ""
    pipe:close()
    assert.truthy(mode:match("^%-rw%-%-%-%-%-%-%-"))
    local line1 = read_file(path):match("^([^\n]*)\n")
    assert.truthy(line1:find('"type":"session"', 1, true))
    assert.truthy(line1:find('"version":1', 1, true))
    assert.truthy(line1:find('"id":"' .. id .. '"', 1, true))
    assert.truthy(line1:find('"created":', 1, true))
    assert.equal(0, tonumber(lib.session_message_count(sess)))
    lib.session_free(sess)
  end)

  it("ids from back-to-back sessions strictly increase", function()
    local a = lib.session_new(err)
    local b = lib.session_new(err)
    assert.is_true(a ~= nil and b ~= nil)
    local ia = ffi.string(lib.session_id(a))
    local ib = ffi.string(lib.session_id(b))
    assert.is_true(ia < ib) -- same second -> the _NN suffix bumped
    lib.session_free(a)
    lib.session_free(b)
  end)

  it("append -> load round-trips byte-identical messages", function()
    local sess = lib.session_new(err)
    local texts = {
      '{"role":"user","content":"héllo ✓ line\\nbreak"}',
      '{"role":"assistant","content":null,"tool_calls":[{"id":"call_1"}]}',
    }
    local expected = {}
    for _, text in ipairs(texts) do
      local msg = parse(text)
      expected[#expected + 1] = printed(msg) -- print before append owns it
      assert.is_true(lib.session_append(sess, msg, err))
    end
    local id = ffi.string(lib.session_id(sess))
    lib.session_free(sess)
    local loaded = lib.session_load(sdir .. "/" .. id .. ".jsonl", err)
    assert.is_true(loaded ~= nil)
    assert.equal(ffi.C.kErrorTypeNone, err.type)
    assert.equal(id, ffi.string(lib.session_id(loaded)))
    assert.equal(2, tonumber(lib.session_message_count(loaded)))
    for i, want in ipairs(expected) do
      assert.equal(want, printed(lib.session_message_get(loaded, i - 1)))
    end
    lib.session_free(loaded)
  end)

  it("append validates shape, owns rejects, and never poisons on them", function()
    local sess = lib.session_new(err)
    assert.is_false(lib.session_append(sess, nil, err))
    assert.equal(ffi.C.kErrorTypeValidation, err.type)
    lib.api_clear_error(err)
    assert.is_false(lib.session_append(sess, parse("42"), err))
    assert.equal(ffi.C.kErrorTypeValidation, err.type)
    lib.api_clear_error(err)
    assert.is_false(lib.session_append(sess, parse('{"norole":true}'), err))
    assert.equal(ffi.C.kErrorTypeValidation, err.type)
    lib.api_clear_error(err)
    -- the rejects are freed (the sanitized run proves it); appends still work
    assert.is_true(lib.session_append(sess, parse('{"role":"user","content":"ok"}'), err))
    assert.equal(1, tonumber(lib.session_message_count(sess)))
    lib.session_free(sess)
  end)

  it("load_latest picks the strcmp-max well-formed name", function()
    put_session("20990101T000000_00.jsonl", header_line("20990101T000000_00") .. "\n")
    put_session("20990101T000000_01.jsonl", header_line("20990101T000000_01") .. "\n")
    put_session("notasession.jsonl", "junk\n") -- 'n' > '2': must lose on shape, not luck
    put_session("99991231T235959_99.jsonl.tmp", "junk\n")
    put_session("zzz.txt", "junk\n")
    local sess = lib.session_load_latest(err)
    assert.is_true(sess ~= nil)
    assert.equal("20990101T000000_01", ffi.string(lib.session_id(sess)))
    lib.session_free(sess)
  end)

  it("load_latest with no sessions errors", function()
    local sess = lib.session_load_latest(err)
    assert.is_true(sess == nil)
    assert.equal(ffi.C.kErrorTypeValidation, err.type)
    assert.truthy(ffi.string(err.msg):find("no sessions", 1, true))
  end)

  it("tolerates a torn tail, heals on append, fails reload naming the line", function()
    local name = "20990101T000000_00.jsonl"
    -- crash mid-write: the tail has no closing brace and no '\n'
    put_session(
      name,
      header_line("20990101T000000_00")
        .. "\n"
        .. '{"role":"user","content":"hi"}\n'
        .. '{"role":"assist'
    )
    local path = sdir .. "/" .. name
    local sess = lib.session_load(path, err)
    assert.is_true(sess ~= nil)
    assert.equal(1, tonumber(lib.session_message_count(sess))) -- torn line skipped
    local msg = parse('{"role":"user","content":"again"}')
    local line = printed(msg)
    assert.is_true(lib.session_append(sess, msg, err))
    lib.session_free(sess)
    local data = read_file(path)
    assert.truthy(data:find('{"role":"assist\n' .. line .. "\n", 1, true)) -- heal byte
    -- the healed torn line now sits mid-file: a reload fails loudly, naming it
    local again = lib.session_load(path, err)
    assert.is_true(again == nil)
    assert.equal(ffi.C.kErrorTypeValidation, err.type)
    assert.truthy(ffi.string(err.msg):find("line 3", 1, true))
  end)

  it("keeps a parseable unterminated tail and heals it", function()
    local name = "20990101T000000_00.jsonl"
    -- a complete record missing only its '\n' (e.g. crash between records)
    put_session(name, header_line("20990101T000000_00") .. "\n" .. '{"role":"user","content":"hi"}')
    local path = sdir .. "/" .. name
    local sess = lib.session_load(path, err)
    assert.is_true(sess ~= nil)
    assert.equal(1, tonumber(lib.session_message_count(sess)))
    assert.is_true(lib.session_append(sess, parse('{"role":"user","content":"more"}'), err))
    lib.session_free(sess)
    local again = lib.session_load(path, err) -- healed file is fully well-formed
    assert.is_true(again ~= nil)
    assert.equal(2, tonumber(lib.session_message_count(again)))
    lib.session_free(again)
  end)

  it("mid-file corruption fails the load naming the line", function()
    put_session(
      "20990101T000000_00.jsonl",
      header_line("20990101T000000_00")
        .. "\n"
        .. "%%%garbage%%%\n"
        .. '{"role":"user","content":"hi"}\n'
    )
    local sess = lib.session_load(sdir .. "/20990101T000000_00.jsonl", err)
    assert.is_true(sess == nil)
    assert.equal(ffi.C.kErrorTypeValidation, err.type)
    assert.truthy(ffi.string(err.msg):find("line 2", 1, true))
  end)

  it("rejects a missing header", function()
    put_session("20990101T000000_00.jsonl", '{"role":"user","content":"hi"}\n')
    assert.is_true(lib.session_load(sdir .. "/20990101T000000_00.jsonl", err) == nil)
    assert.equal(ffi.C.kErrorTypeValidation, err.type)
  end)

  it("rejects an unsupported header version", function()
    put_session(
      "20990101T000000_00.jsonl",
      '{"type":"session","version":2,"id":"20990101T000000_00","created":1}\n'
    )
    assert.is_true(lib.session_load(sdir .. "/20990101T000000_00.jsonl", err) == nil)
    assert.equal(ffi.C.kErrorTypeValidation, err.type)
  end)

  it("rejects an empty file", function()
    put_session("20990101T000000_00.jsonl", "")
    assert.is_true(lib.session_load(sdir .. "/20990101T000000_00.jsonl", err) == nil)
    assert.equal(ffi.C.kErrorTypeValidation, err.type)
  end)

  it("tolerates unknown control records and blank lines", function()
    put_session(
      "20990101T000000_00.jsonl",
      header_line("20990101T000000_00")
        .. "\n"
        .. '{"type":"meta","usage":{}}\n'
        .. "\n"
        .. '{"role":"user","content":"hi"}\n'
    )
    local sess = lib.session_load(sdir .. "/20990101T000000_00.jsonl", err)
    assert.is_true(sess ~= nil)
    assert.equal(1, tonumber(lib.session_message_count(sess)))
    lib.session_free(sess)
  end)

  it("caps a load at 4096 messages", function()
    local parts = { header_line("20990101T000000_00") }
    for i = 1, 4097 do
      parts[#parts + 1] = '{"role":"user","content":"' .. i .. '"}'
    end
    put_session("20990101T000000_00.jsonl", table.concat(parts, "\n") .. "\n")
    assert.is_true(lib.session_load(sdir .. "/20990101T000000_00.jsonl", err) == nil)
    assert.equal(ffi.C.kErrorTypeValidation, err.type)
  end)

  it("refuses an oversize append and leaves the file untouched", function()
    local sess = lib.session_new(err)
    local path = sdir .. "/" .. ffi.string(lib.session_id(sess)) .. ".jsonl"
    local before = #read_file(path)
    local msg = parse('{"role":"user","content":"' .. string.rep("x", 9 * 1024 * 1024) .. '"}')
    assert.is_false(lib.session_append(sess, msg, err))
    assert.equal(ffi.C.kErrorTypeValidation, err.type)
    assert.equal(before, #read_file(path))
    -- oversize is validation, not I/O: the session is not poisoned
    assert.is_true(lib.session_append(sess, parse('{"role":"user","content":"ok"}'), err))
    lib.session_free(sess)
  end)
end)

-- The current-session registry (handle resolution; 0 = current). session_resolve
-- never dereferences the pointer, so a sentinel proves the wiring with no real
-- session and no filesystem. The global is process-shared; reset it around use.
describe("session_resolve", function()
  it("resolves 0 to the current session and errors on absent/unknown handles", function()
    local err = new_error()
    lib.session_set_current(nil)
    assert.is_true(lib.session_resolve(0, err) == nil) -- no current session
    assert.equal(ffi.C.kErrorTypeValidation, err.type)
    assert.truthy(ffi.string(err.msg):find("no current session", 1, true))
    lib.api_clear_error(err)

    local sentinel = ffi.cast("SessionState *", 0x1234)
    lib.session_set_current(sentinel)
    assert.is_true(lib.session_resolve(0, err) == sentinel) -- 0 -> current
    assert.equal(ffi.C.kErrorTypeNone, err.type)

    assert.is_true(lib.session_resolve(5, err) == nil) -- no multi-session table yet
    assert.equal(ffi.C.kErrorTypeValidation, err.type)
    assert.truthy(ffi.string(err.msg):find("unknown session handle", 1, true))
    lib.api_clear_error(err)

    lib.session_set_current(nil) -- leave the process-global clean for siblings
  end)
end)
