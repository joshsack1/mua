local t = require("test.unit.helpers")
local ffi = t.ffi
local lib = t.lib

-- Declarations mirror src/mua/json.h (cJSON stays opaque on this side; the
-- typedef and parse/print/free/init come from helpers).
t.cdef([[
  const char *json_get_cstr(const cJSON *obj, const char *key);
  bool json_get_int(const cJSON *obj, const char *key, int64_t *out);
  bool json_get_bool(const cJSON *obj, const char *key, bool *out);
  cJSON *json_get_obj(const cJSON *obj, const char *key);
  cJSON *json_get_arr(const cJSON *obj, const char *key);
  cJSON *json_new_obj(void);
  void json_add_str(cJSON *obj, const char *key, String val);
  void json_add_cstr(cJSON *obj, const char *key, const char *val);
  void json_add_int(cJSON *obj, const char *key, int64_t val);
  void json_add_bool(cJSON *obj, const char *key, bool val);
  cJSON *json_add_arr(cJSON *obj, const char *key);

  cJSON *cJSON_GetArrayItem(const cJSON *array, int index);
  int cJSON_GetArraySize(const cJSON *array);
  cJSON *cJSON_CreateObject(void);
  void cJSON_AddItemToArray(cJSON *array, cJSON *item);
]])

lib.json_init()

local anchors = {}

--- Build a String view over a Lua string (anchored against GC for the test).
local function S(str)
  anchors[#anchors + 1] = str
  return ffi.new("String", ffi.cast("char *", str), #str)
end

local function parse(doc, cap)
  local err = ffi.new("Error")
  err.type = ffi.C.kErrorTypeNone
  local node = lib.json_parse(S(doc), cap or 1048576, err)
  local errtype = err.type
  local msg = nil
  if err.msg ~= nil then
    msg = ffi.string(err.msg)
  end
  lib.api_clear_error(err)
  return node, errtype, msg
end

describe("json_parse", function()
  after_each(function()
    anchors = {}
  end)

  it("rejects a 65-deep nesting bomb cleanly (CJSON_NESTING_LIMIT=64)", function()
    local bomb = ("["):rep(65) .. "1" .. ("]"):rep(65)
    local node, _, msg = parse(bomb)
    assert.is_true(node == nil)
    assert.truthy(msg:match("parse failed"))
  end)

  it("accepts depth exactly 64 (the boundary)", function()
    local ok64 = ("["):rep(64) .. "1" .. ("]"):rep(64)
    local node = parse(ok64)
    assert.is_true(node ~= nil)
    lib.json_free(node)
  end)

  it("rejects documents over the size cap before parsing", function()
    local big = "[" .. ("1,"):rep(1000) .. "1]"
    local node, errtype, msg = parse(big, 1024)
    assert.is_true(node == nil)
    assert.equal(ffi.C.kErrorTypeValidation, errtype)
    assert.truthy(msg:match("2003 bytes"))
    assert.truthy(msg:match("cap of 1024"))
  end)

  it("errors (never crashes) on malformed input", function()
    for _, doc in ipairs({ '{"a":', "zzz", "", "{", '["unterminated' }) do
      local node = parse(doc)
      assert.is_true(node == nil, "expected failure for: " .. doc)
    end
  end)

  it("errors on a NULL document", function()
    local err = ffi.new("Error")
    err.type = ffi.C.kErrorTypeNone
    local node = lib.json_parse(ffi.new("String"), 1024, err)
    assert.is_true(node == nil)
    assert.truthy(ffi.string(err.msg):match("empty document"))
    lib.api_clear_error(err)
  end)

  it("honors size over NUL-termination and rejects trailing garbage", function()
    local raw = '{"a":1}GARBAGE'
    anchors[#anchors + 1] = raw
    -- A size-bounded view over the valid prefix parses: the parser reads
    -- exactly 7 bytes and never touches the garbage beyond them.
    local exact = ffi.new("String", ffi.cast("char *", raw), 7)
    local err = ffi.new("Error")
    err.type = ffi.C.kErrorTypeNone
    local good = lib.json_parse(exact, 1024, err)
    assert.is_true(good ~= nil)
    local out = ffi.new("int64_t[1]")
    assert.is_true(lib.json_get_int(good, "a", out))
    assert.equal(1, tonumber(out[0]))
    lib.json_free(good)
    -- The same bytes WITH the garbage inside the size are rejected: an event
    -- carries exactly one JSON document.
    local node = parse(raw)
    assert.is_true(node == nil)
  end)
end)

describe("getters", function()
  it("extracts an OpenRouter delta chunk", function()
    local node = parse('{"choices":[{"delta":{"content":"Hel"},"finish_reason":null}]}')
    assert.is_true(node ~= nil)
    local choices = lib.json_get_arr(node, "choices")
    assert.is_true(choices ~= nil)
    assert.equal(1, lib.cJSON_GetArraySize(choices))
    local delta = lib.json_get_obj(lib.cJSON_GetArrayItem(choices, 0), "delta")
    assert.is_true(delta ~= nil)
    assert.equal("Hel", ffi.string(lib.json_get_cstr(delta, "content")))
    -- finish_reason is JSON null: not a string, so the getter reports missing.
    assert.is_true(lib.json_get_cstr(lib.cJSON_GetArrayItem(choices, 0), "finish_reason") == nil)
    lib.json_free(node)
  end)

  it("extracts an OpenRouter error envelope", function()
    local node = parse('{"error":{"message":"Insufficient credits","code":402}}')
    local errobj = lib.json_get_obj(node, "error")
    assert.is_true(errobj ~= nil)
    assert.equal("Insufficient credits", ffi.string(lib.json_get_cstr(errobj, "message")))
    local code = ffi.new("int64_t[1]")
    assert.is_true(lib.json_get_int(errobj, "code", code))
    assert.equal(402, tonumber(code[0]))
    lib.json_free(node)
  end)

  it("reports type mismatches as missing and leaves out untouched", function()
    local node = parse('{"s":"text","n":5,"b":true}')
    local out = ffi.new("int64_t[1]", 99)
    assert.is_false(lib.json_get_int(node, "s", out))
    assert.equal(99, tonumber(out[0]))
    local flag = ffi.new("bool[1]", false)
    assert.is_true(lib.json_get_bool(node, "b", flag))
    assert.is_true(flag[0])
    assert.is_false(lib.json_get_bool(node, "n", flag))
    assert.is_true(lib.json_get_obj(node, "s") == nil)
    assert.is_true(lib.json_get_arr(node, "s") == nil)
    lib.json_free(node)
  end)
end)

describe("builders and print", function()
  it("locks the request wire format", function()
    local body = lib.json_new_obj()
    lib.json_add_cstr(body, "model", "anthropic/claude-sonnet-4.6")
    lib.json_add_bool(body, "stream", true)
    local messages = lib.json_add_arr(body, "messages")
    local msg = lib.cJSON_CreateObject()
    lib.json_add_cstr(msg, "role", "user")
    lib.json_add_str(msg, "content", S("hi there"))
    lib.cJSON_AddItemToArray(messages, msg)
    local printed = lib.json_print(body)
    assert.equal(
      '{"model":"anthropic/claude-sonnet-4.6","stream":true,'
        .. '"messages":[{"role":"user","content":"hi there"}]}',
      ffi.string(printed.data, printed.size)
    )
    lib.xfree(printed.data)
    lib.json_free(body)
  end)

  it("json_free(NULL) is safe", function()
    lib.json_free(nil)
  end)
end)
