local t = require("test.unit.helpers")
local ffi = t.ffi
local lib = t.lib

-- The pure parse step of the /models fetch; String/json_init come from helpers.
t.cdef([[
  int64_t models_parse_context_length(String body, const char *model_id);
]])

local anchors = {}

local function S(str)
  anchors[#anchors + 1] = str
  return ffi.new("String", ffi.cast("char *", str), #str)
end

local function parse(json, id)
  return tonumber(lib.models_parse_context_length(S(json), id))
end

describe("models catalog parse", function()
  before_each(function()
    lib.json_init() -- route cJSON through xmalloc/xfree (idempotent)
    anchors = {}
  end)

  it("prefers top_provider.context_length over the top-level field", function()
    local body =
      '{"data":[{"id":"x/y","context_length":4096,"top_provider":{"context_length":8000}}]}'
    assert.equal(8000, parse(body, "x/y"))
  end)

  it("falls back to the top-level field when top_provider lacks it", function()
    assert.equal(4096, parse('{"data":[{"id":"x/y","context_length":4096,"top_provider":{}}]}', "x/y"))
    assert.equal(4096, parse('{"data":[{"id":"x/y","context_length":4096}]}', "x/y"))
  end)

  it("finds the right model among several", function()
    local body = '{"data":['
      .. '{"id":"a/b","context_length":100},'
      .. '{"id":"x/y","context_length":4096,"top_provider":{"context_length":8000}},'
      .. '{"id":"c/d","context_length":200}]}'
    assert.equal(8000, parse(body, "x/y"))
  end)

  it("returns 0 for absent model, missing/zero field, or unparseable body", function()
    assert.equal(0, parse('{"data":[{"id":"a/b","context_length":1000}]}', "x/y")) -- no match
    assert.equal(0, parse('{"data":[{"id":"x/y"}]}', "x/y")) -- field missing
    assert.equal(0, parse('{"data":[{"id":"x/y","context_length":0}]}', "x/y")) -- non-positive
    assert.equal(0, parse('{"no_data":true}', "x/y")) -- no data array
    assert.equal(0, parse("not json", "x/y")) -- unparseable
    assert.equal(0, parse('{"data":[{"id":"x/y","context_length":4096}]}', "")) -- empty id
  end)
end)
