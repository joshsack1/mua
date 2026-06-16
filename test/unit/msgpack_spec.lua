local t = require("test.unit.helpers")
local ffi = t.ffi
local lib = t.lib

-- The cJSON<->Object converters are shared with marshal_spec; kept in their own
-- block so a redefine-abort (if that spec ran first) leaves nothing missing.
t.cdef([[
  void api_free_object(Object *obj);
  bool cjson_to_object(const cJSON *node, Object *out, Error *err);
  cJSON *object_to_cjson(const Object *obj);
]])

-- The msgpack codec symbols are unique to this spec.
t.cdef([[
  typedef struct { uint8_t *data; size_t size; size_t cap; } MsgpackBuffer;
  void msgpack_buffer_free(MsgpackBuffer *buf);
  void msgpack_encode(MsgpackBuffer *buf, const Object *obj);
  typedef enum { kMsgpackOk = 0, kMsgpackIncomplete, kMsgpackError } MsgpackStatus;
  MsgpackStatus msgpack_to_object(const uint8_t *buf, size_t len, size_t *consumed,
                                  Object *out, Error *err);
]])

lib.json_init()

local function new_error()
  local err = ffi.new("Error")
  err.type = ffi.C.kErrorTypeNone
  return err
end

local function parse(json)
  local buf = ffi.new("char[?]", #json + 1, json)
  local doc = ffi.new("String", { data = buf, size = #json })
  return lib.json_parse(doc, 1024 * 1024, new_error())
end

-- A uint8_t[] view over raw bytes (anchored by the caller for the call's life).
local function bytes(s)
  return ffi.new("uint8_t[?]", #s, s)
end

-- json -> cjson_to_object -> msgpack_encode -> msgpack_to_object -> object_to_cjson
-- -> print: the full Object<->msgpack edge, asserting *consumed spans the buffer.
local function roundtrip(json)
  local node = parse(json)
  local o = ffi.new("Object[1]")
  assert.is_true(lib.cjson_to_object(node, o, new_error()))
  lib.json_free(node)

  local buf = ffi.new("MsgpackBuffer")
  lib.msgpack_encode(buf, o)
  lib.api_free_object(o)

  local out = ffi.new("Object[1]")
  local consumed = ffi.new("size_t[1]")
  local st = lib.msgpack_to_object(buf.data, buf.size, consumed, out, new_error())
  assert.equal(ffi.C.kMsgpackOk, st)
  assert.equal(tonumber(buf.size), tonumber(consumed[0]))
  lib.msgpack_buffer_free(buf)

  local back = lib.object_to_cjson(out)
  lib.api_free_object(out)
  local s = lib.json_print(back)
  lib.json_free(back)
  local str = ffi.string(s.data, s.size)
  lib.xfree(s.data)
  return str
end

-- Decodes raw msgpack bytes; returns (status, Object[1], consumed[1], err).
local function decode(raw)
  local out = ffi.new("Object[1]")
  local consumed = ffi.new("size_t[1]")
  local err = new_error()
  local st = lib.msgpack_to_object(bytes(raw), #raw, consumed, out, err)
  return st, out, consumed, err
end

describe("Object <-> msgpack round-trip", function()
  it("preserves scalars, arrays, and nested maps", function()
    assert.equal('[1,2.5,true,"x",null]', roundtrip('[1,2.5,true,"x",null]'))
    assert.equal('{"a":1,"b":[2,3],"c":"hi"}', roundtrip('{"a":1,"b":[2,3],"c":"hi"}'))
    assert.equal("{}", roundtrip("{}"))
    assert.equal("[]", roundtrip("[]"))
    assert.equal("[-1,-33,-32768,-2147483648,127,128,65536]",
      roundtrip("[-1,-33,-32768,-2147483648,127,128,65536]")) -- exercises every int width
  end)

  it("preserves the wide str8 / array16 / map16 headers", function()
    local big_str = string.rep("x", 200) -- > 31 bytes -> str8
    assert.equal('["' .. big_str .. '"]', roundtrip('["' .. big_str .. '"]'))

    local nums = {}
    for i = 0, 19 do
      nums[#nums + 1] = tostring(i) -- 20 elements -> array16
    end
    local arr = "[" .. table.concat(nums, ",") .. "]"
    assert.equal(arr, roundtrip(arr))

    local pairs_ = {}
    for i = 0, 19 do
      pairs_[#pairs_ + 1] = ('"k%d":%d'):format(i, i) -- 20 keys -> map16 (order preserved)
    end
    local map = "{" .. table.concat(pairs_, ",") .. "}"
    assert.equal(map, roundtrip(map))
  end)
end)

describe("msgpack_to_object decoding", function()
  it("consumes only the first value, leaving trailing bytes", function()
    local st, out, consumed = decode("\x01\xff") -- int 1, then a stray byte
    assert.equal(ffi.C.kMsgpackOk, st)
    assert.equal(1, tonumber(consumed[0]))
    assert.equal(ffi.C.kObjectTypeInteger, out[0].type)
    assert.equal(1, tonumber(out[0].data.integer))
    lib.api_free_object(out)
  end)

  it("reports a truncated value as incomplete (no allocation)", function()
    -- fixstr of length 5 with only 2 payload bytes present
    assert.equal(ffi.C.kMsgpackIncomplete, (decode("\xa5ab")))
    -- fixarray of 1 element with no element bytes: count > remaining
    assert.equal(ffi.C.kMsgpackIncomplete, (decode("\x91")))
    -- an empty buffer
    assert.equal(ffi.C.kMsgpackIncomplete, (decode("")))
  end)

  it("rejects unmodeled types and non-string map keys", function()
    local st, _, _, err = decode("\xc1") -- never-used
    assert.equal(ffi.C.kMsgpackError, st)
    assert.equal(ffi.C.kErrorTypeValidation, err.type)
    lib.api_clear_error(err)

    assert.equal(ffi.C.kMsgpackError, (decode("\xd4\x00\x00"))) -- fixext1 (ext family)

    -- fixmap of one pair whose key is an integer, not a string
    st = decode("\x81\x01\x02")
    assert.equal(ffi.C.kMsgpackError, st)
  end)

  it("rejects nesting past the depth cap without leaking", function()
    local deep = string.rep("\x91", 40) .. "\x01" -- 40 nested 1-element arrays
    local st, _, _, err = decode(deep)
    assert.equal(ffi.C.kMsgpackError, st)
    assert.equal(ffi.C.kErrorTypeValidation, err.type)
    assert.truthy(ffi.string(err.msg):find("nesting", 1, true))
    lib.api_clear_error(err) -- the partial tree was freed inside the decoder (ASan checks)
  end)
end)
