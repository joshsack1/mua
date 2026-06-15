local t = require("test.unit.helpers")
local ffi = t.ffi
local lib = t.lib

-- The Object vocabulary and String/Error come from the shared helpers cdef. The
-- store is a process-global singleton and busted runs one process, so every case
-- resets it via variables_free in before_each. The Lua<->Object marshaling that
-- backs mua.g is static in the bridge and not reachable here; it is covered by
-- the functional vars_spec round-trips. These cases drive the store directly.
t.cdef([[
  void api_free_string(String str);
  void api_free_object(Object *obj);
  void variables_set(String name, Object value, Error *err);
  Object variables_get(String name, Error *err);
  void variables_free(void);
]])

local VAR_MAX = 1024 -- mirrors MUA_VAR_MAX in variables.h

local function new_error()
  local err = ffi.new("Error")
  err.type = ffi.C.kErrorTypeNone
  return err
end

local function set_int(name, n)
  local nbuf = ffi.new("char[?]", #name + 1, name)
  local nm = ffi.new("String", { data = nbuf, size = #name })
  local o = ffi.new("Object")
  o.type = ffi.C.kObjectTypeInteger
  o.data.integer = n
  local err = new_error()
  lib.variables_set(nm, o, err)
  return err
end

local function set_str(name, value)
  local nbuf = ffi.new("char[?]", #name + 1, name)
  local vbuf = ffi.new("char[?]", #value + 1, value)
  local nm = ffi.new("String", { data = nbuf, size = #name })
  local o = ffi.new("Object")
  o.type = ffi.C.kObjectTypeString
  o.data.string.data = vbuf
  o.data.string.size = #value
  local err = new_error()
  lib.variables_set(nm, o, err)
  return err
end

local function set_nil(name)
  local nbuf = ffi.new("char[?]", #name + 1, name)
  local nm = ffi.new("String", { data = nbuf, size = #name })
  local o = ffi.new("Object") -- zero-initialized => kObjectTypeNil
  local err = new_error()
  lib.variables_set(nm, o, err)
  return err
end

-- Returns the value as an Object[1] (so api_free_object can take its address)
-- plus the error.
local function get(name)
  local nbuf = ffi.new("char[?]", #name + 1, name)
  local nm = ffi.new("String", { data = nbuf, size = #name })
  local err = new_error()
  local o = ffi.new("Object[1]")
  o[0] = lib.variables_get(nm, err)
  return o, err
end

local function is_validation(err)
  return err.type == ffi.C.kErrorTypeValidation
end

describe("variables store", function()
  before_each(function()
    lib.variables_free() -- isolate cases: one process, one shared singleton
  end)

  it("round-trips an integer", function()
    local err = set_int("count", 7)
    assert.equal(ffi.C.kErrorTypeNone, err.type)
    local o = get("count")
    assert.equal(ffi.C.kObjectTypeInteger, o[0].type)
    assert.equal(7, tonumber(o[0].data.integer))
  end)

  it("round-trips a string and returns an owned copy", function()
    set_str("name", "hi")
    local o = get("name")
    assert.equal(ffi.C.kObjectTypeString, o[0].type)
    assert.equal("hi", ffi.string(o[0].data.string.data, o[0].data.string.size))
    lib.api_free_object(o)
  end)

  it("deep-copies a nested array value", function()
    local items = ffi.new("Object[2]")
    items[0].type = ffi.C.kObjectTypeInteger
    items[0].data.integer = 10
    items[1].type = ffi.C.kObjectTypeInteger
    items[1].data.integer = 20
    local nbuf = ffi.new("char[?]", 4, "arr")
    local nm = ffi.new("String", { data = nbuf, size = 3 })
    local val = ffi.new("Object")
    val.type = ffi.C.kObjectTypeArray
    val.data.array.items = items
    val.data.array.size = 2
    val.data.array.capacity = 2
    local err = new_error()
    lib.variables_set(nm, val, err)
    assert.equal(ffi.C.kErrorTypeNone, err.type)

    local o = get("arr")
    assert.equal(ffi.C.kObjectTypeArray, o[0].type)
    assert.equal(2, tonumber(o[0].data.array.size))
    assert.is_true(o[0].data.array.items ~= items) -- deep copy, distinct storage
    assert.equal(10, tonumber(o[0].data.array.items[0].data.integer))
    assert.equal(20, tonumber(o[0].data.array.items[1].data.integer))
    lib.api_free_object(o)
  end)

  it("deletes a key when set to Nil", function()
    set_int("x", 1)
    set_nil("x")
    local o = get("x")
    assert.equal(ffi.C.kObjectTypeNil, o[0].type)
  end)

  it("setting an unset key to Nil is a no-op", function()
    local err = set_nil("never")
    assert.equal(ffi.C.kErrorTypeNone, err.type)
  end)

  it("returns Nil and no error for an unset key", function()
    local o, err = get("missing")
    assert.equal(ffi.C.kObjectTypeNil, o[0].type)
    assert.equal(ffi.C.kErrorTypeNone, err.type) -- unlike options_get, not an error
  end)

  it("overwrites a prior value of a different type", function()
    set_int("k", 1)
    set_str("k", "now-a-string")
    local o = get("k")
    assert.equal(ffi.C.kObjectTypeString, o[0].type)
    assert.equal("now-a-string", ffi.string(o[0].data.string.data, o[0].data.string.size))
    lib.api_free_object(o)
  end)

  it("returns owned copies safe to free independently", function()
    set_str("k", "owned/copy")
    local a = get("k")
    local b = get("k")
    assert.is_true(a[0].data.string.data ~= b[0].data.string.data) -- distinct allocations
    lib.api_free_object(a)
    lib.api_free_object(b) -- ASan would catch a double free
    local c = get("k")
    assert.equal("owned/copy", ffi.string(c[0].data.string.data, c[0].data.string.size))
    lib.api_free_object(c) -- store master intact
  end)

  it("rejects a new key past the entry cap", function()
    for i = 1, VAR_MAX do
      local err = set_int("k" .. i, i)
      assert.equal(ffi.C.kErrorTypeNone, err.type)
    end
    local err = set_int("one-too-many", 0)
    assert.is_true(is_validation(err))
    assert.truthy(ffi.string(err.msg):find("too many", 1, true))
    lib.api_clear_error(err)
  end)
end)
