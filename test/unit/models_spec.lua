local t = require("test.unit.helpers")
local ffi = t.ffi
local lib = t.lib

-- The model-catalog cache's pure seams: populate the cache from a JSON body, then
-- read a cached window. One populate caches every model in the body, so any can
-- be looked up afterward. String/json_init come from helpers.
t.cdef([[
  void models_cache_free(void);
  void models_cache_populate(String body);
  int64_t models_cache_window(const char *model_id);
]])

local anchors = {}

local function S(str)
  anchors[#anchors + 1] = str
  return ffi.new("String", ffi.cast("char *", str), #str)
end

local function populate(json)
  lib.models_cache_populate(S(json))
end

local function window(id)
  return tonumber(lib.models_cache_window(id))
end

describe("models catalog cache", function()
  before_each(function()
    lib.json_init()         -- route cJSON through xmalloc/xfree (idempotent)
    lib.models_cache_free() -- isolate: start each case with an empty cache
    anchors = {}
  end)

  it("prefers top_provider.context_length over the top-level field", function()
    populate('{"data":[{"id":"x/y","context_length":4096,"top_provider":{"context_length":8000}}]}')
    assert.equal(8000, window("x/y"))
  end)

  it("falls back to the top-level field when top_provider lacks it", function()
    populate('{"data":[{"id":"x/y","context_length":4096,"top_provider":{}}]}')
    assert.equal(4096, window("x/y"))
    populate('{"data":[{"id":"x/y","context_length":4096}]}') -- repopulate resets the cache
    assert.equal(4096, window("x/y"))
  end)

  it("caches every model so any can be looked up after one populate", function()
    populate('{"data":['
      .. '{"id":"a/b","context_length":100},'
      .. '{"id":"x/y","context_length":4096,"top_provider":{"context_length":8000}},'
      .. '{"id":"c/d","context_length":200}]}')
    assert.equal(100, window("a/b"))
    assert.equal(8000, window("x/y"))
    assert.equal(200, window("c/d"))
  end)

  it("returns 0 for an absent model, missing/zero field, or unparseable body", function()
    populate('{"data":[{"id":"a/b","context_length":1000}]}')
    assert.equal(0, window("x/y")) -- not in the catalog
    populate('{"data":[{"id":"x/y"}]}')
    assert.equal(0, window("x/y")) -- field missing
    populate('{"data":[{"id":"x/y","context_length":0}]}')
    assert.equal(0, window("x/y")) -- non-positive
    populate('{"no_data":true}')
    assert.equal(0, window("x/y")) -- no data array
    populate("not json")
    assert.equal(0, window("x/y")) -- unparseable body
    assert.equal(0, window("")) -- empty id never matches
  end)
end)
