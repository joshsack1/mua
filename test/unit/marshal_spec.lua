local t = require("test.unit.helpers")
local ffi = t.ffi
local lib = t.lib

-- cjson_to_object / object_to_cjson are the one-place cJSON<->Object converters
-- (json.h boundary rule), reachable directly via FFI -- unlike the Object<->Lua
-- marshaling in the bridge, which is static and covered by functional specs. The
-- cJSON typedef and the parse/print/free/init quartet come from the shared
-- helpers cdef; the Object vocabulary does too.
t.cdef([[
  void api_free_object(Object *obj);
  bool cjson_to_object(const cJSON *node, Object *out, Error *err);
  cJSON *object_to_cjson(const Object *obj);
]])

lib.json_init() -- route cJSON through xmalloc/xfree, as the real process does

local function new_error()
  local err = ffi.new("Error")
  err.type = ffi.C.kErrorTypeNone
  return err
end

-- Parses `json` into a cJSON tree. The input buffer only needs to outlive the
-- call (cJSON copies), so it is safe to drop once parse returns.
local function parse(json)
  local buf = ffi.new("char[?]", #json + 1, json)
  local doc = ffi.new("String", { data = buf, size = #json })
  return lib.json_parse(doc, 65536, new_error())
end

-- Finds a Dict entry's value Object by key (the converter preserves child order,
-- but tests should not depend on it).
local function dict_get(obj, key)
  for i = 0, tonumber(obj.data.dict.size) - 1 do
    local k = obj.data.dict.items[i].key
    if ffi.string(k.data, k.size) == key then
      return obj.data.dict.items[i].value
    end
  end
  return nil
end

-- parse -> cjson_to_object -> object_to_cjson -> print, the full edge.
local function roundtrip(json)
  local node = parse(json)
  local o = ffi.new("Object[1]")
  assert.is_true(lib.cjson_to_object(node, o, new_error()))
  lib.json_free(node)
  local back = lib.object_to_cjson(o[0])
  lib.api_free_object(o)
  local s = lib.json_print(back)
  lib.json_free(back)
  local str = ffi.string(s.data, s.size)
  lib.xfree(s.data)
  return str
end

describe("cjson_to_object", function()
  it("builds a heap Object tree from a cJSON object", function()
    local node = parse('{"a":1,"b":"x","c":[true,2.5],"d":null}')
    local o = ffi.new("Object[1]")
    local err = new_error()
    assert.is_true(lib.cjson_to_object(node, o, err))
    lib.json_free(node)

    assert.equal(ffi.C.kObjectTypeDict, o[0].type)
    assert.equal(4, tonumber(o[0].data.dict.size))

    local a = dict_get(o[0], "a")
    assert.equal(ffi.C.kObjectTypeInteger, a.type)
    assert.equal(1, tonumber(a.data.integer))

    local b = dict_get(o[0], "b")
    assert.equal(ffi.C.kObjectTypeString, b.type)
    assert.equal("x", ffi.string(b.data.string.data, b.data.string.size))

    local c = dict_get(o[0], "c")
    assert.equal(ffi.C.kObjectTypeArray, c.type)
    assert.equal(2, tonumber(c.data.array.size))
    assert.equal(ffi.C.kObjectTypeBoolean, c.data.array.items[0].type)
    assert.is_true(c.data.array.items[0].data.boolean)
    assert.equal(ffi.C.kObjectTypeFloat, c.data.array.items[1].type)
    assert.equal(2.5, tonumber(c.data.array.items[1].data.floating))

    local d = dict_get(o[0], "d")
    assert.equal(ffi.C.kObjectTypeNil, d.type) -- JSON null -> Nil

    lib.api_free_object(o)
  end)

  it("splits numbers into integer and float by the 2^53 rule", function()
    local node = parse("[1, 2.5, 1e20]")
    local o = ffi.new("Object[1]")
    assert.is_true(lib.cjson_to_object(node, o, new_error()))
    lib.json_free(node)
    local items = o[0].data.array.items
    assert.equal(ffi.C.kObjectTypeInteger, items[0].type) -- exact integer
    assert.equal(ffi.C.kObjectTypeFloat, items[1].type) -- fractional
    assert.equal(ffi.C.kObjectTypeFloat, items[2].type) -- 1e20 > 2^53
    lib.api_free_object(o)
  end)

  it("rejects nesting past the depth cap without leaking the partial tree", function()
    local deep = string.rep("[", 40) .. "1" .. string.rep("]", 40)
    local node = parse(deep)
    assert.is_true(node ~= ffi.NULL) -- cJSON's own limit is deeper than ours
    local o = ffi.new("Object[1]")
    local err = new_error()
    local ok = lib.cjson_to_object(node, o, err)
    lib.json_free(node)
    assert.is_false(ok)
    assert.equal(ffi.C.kErrorTypeValidation, err.type)
    assert.truthy(ffi.string(err.msg):find("nesting", 1, true))
    lib.api_clear_error(err) -- the partial tree was freed inside the converter (ASan checks)
  end)
end)

describe("object_to_cjson", function()
  it("round-trips arrays and nested objects through both converters", function()
    assert.equal('[1,2.5,true,"x",null]', roundtrip('[1,2.5,true,"x",null]'))
    assert.equal('{"a":1,"b":[2,3]}', roundtrip('{"a":1,"b":[2,3]}'))
    assert.equal("{}", roundtrip("{}"))
    assert.equal("[]", roundtrip("[]"))
  end)

  it("encodes scalar Objects directly", function()
    local o = ffi.new("Object[1]") -- zero-initialized => Nil
    local j = lib.object_to_cjson(o)
    local s = lib.json_print(j)
    assert.equal("null", ffi.string(s.data, s.size))
    lib.xfree(s.data)
    lib.json_free(j)

    o[0].type = ffi.C.kObjectTypeBoolean
    o[0].data.boolean = true
    j = lib.object_to_cjson(o)
    s = lib.json_print(j)
    assert.equal("true", ffi.string(s.data, s.size))
    lib.xfree(s.data)
    lib.json_free(j)
  end)
end)
