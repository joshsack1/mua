local t = require("test.unit.helpers")
local ffi = t.ffi
local lib = t.lib

-- Declarations mirror src/mua/api/private/helpers.h (types come from helpers).
t.cdef([[
  void api_set_error(Error *err, ErrorType type, const char *fmt, ...);
  String cstr_to_string(const char *str);
  String cstr_as_string(const char *str);
  void api_free_string(String str);
  String api_string_dup(String s);
  Object api_copy_object(const Object *src);
  void api_free_object(Object *obj);
]])

local function new_error()
  local err = ffi.new("Error")
  err.type = ffi.C.kErrorTypeNone
  return err
end

describe("String helpers", function()
  it("cstr_to_string copies with size and kept NUL", function()
    local s = lib.cstr_to_string("abc")
    assert.equal(3, tonumber(s.size))
    assert.equal("abc", ffi.string(s.data, s.size))
    assert.equal(0, s.data[3]) -- NUL kept for convenience
    lib.api_free_string(s)
  end)

  it("cstr_to_string(NULL) is STRING_INIT", function()
    local s = lib.cstr_to_string(nil)
    assert.is_true(s.data == nil)
    assert.equal(0, tonumber(s.size))
  end)

  it("cstr_as_string is a zero-copy view", function()
    local src = ffi.new("char[4]", "xyz")
    local s = lib.cstr_as_string(src)
    assert.is_true(s.data == src)
    assert.equal(3, tonumber(s.size))
  end)
end)

describe("api_set_error", function()
  it("formats the message and sets the type", function()
    local err = new_error()
    lib.api_set_error(err, ffi.C.kErrorTypeValidation, "bad %s: %d", "thing", ffi.cast("int", 42))
    assert.equal(ffi.C.kErrorTypeValidation, err.type)
    assert.equal("bad thing: 42", ffi.string(err.msg))
    lib.api_clear_error(err)
  end)

  it("overwriting releases the prior message", function()
    local err = new_error()
    lib.api_set_error(err, ffi.C.kErrorTypeException, "first")
    lib.api_set_error(err, ffi.C.kErrorTypeValidation, "second")
    assert.equal("second", ffi.string(err.msg))
    assert.equal(ffi.C.kErrorTypeValidation, err.type)
    lib.api_clear_error(err)
  end)

  it("api_clear_error resets to ERROR_INIT and is idempotent", function()
    local err = new_error()
    lib.api_set_error(err, ffi.C.kErrorTypeException, "boom")
    lib.api_clear_error(err)
    assert.equal(ffi.C.kErrorTypeNone, err.type)
    assert.is_true(err.msg == nil)
    lib.api_clear_error(err)
    assert.equal(ffi.C.kErrorTypeNone, err.type)
  end)
end)

describe("api_string_dup", function()
  it("deep-copies with size and a kept trailing NUL", function()
    local buf = ffi.new("char[6]", "hello")
    local s = ffi.new("String", { data = buf, size = 5 })
    local d = lib.api_string_dup(s)
    assert.equal(5, tonumber(d.size))
    assert.equal("hello", ffi.string(d.data, d.size))
    assert.equal(0, d.data[5]) -- trailing NUL kept for convenience
    assert.is_true(d.data ~= buf) -- distinct allocation
    lib.api_free_string(d)
  end)

  it("maps NULL data to STRING_INIT", function()
    local s = ffi.new("String", { data = nil, size = 0 })
    local d = lib.api_string_dup(s)
    assert.is_true(d.data == nil)
    assert.equal(0, tonumber(d.size))
  end)
end)

-- These build a SOURCE Object out of LuaJIT-owned (GC) buffers and pass it to
-- api_copy_object, which only reads it; the returned copy is fully xmalloc'd, so
-- api_free_object is the one that must release it. Under the SANITIZE build ASan
-- is the real assertion: a missed free leaks, an over-free or stray free of a
-- GC buffer aborts. The source is never handed to api_free_object.
describe("api_copy_object / api_free_object", function()
  it("copies a scalar by value; free is a no-op", function()
    local src = ffi.new("Object[1]")
    src[0].type = ffi.C.kObjectTypeInteger
    src[0].data.integer = 7
    local dst = ffi.new("Object[1]")
    dst[0] = lib.api_copy_object(src)
    assert.equal(ffi.C.kObjectTypeInteger, dst[0].type)
    assert.equal(7, tonumber(dst[0].data.integer))
    lib.api_free_object(dst)
    assert.equal(ffi.C.kObjectTypeNil, dst[0].type)
  end)

  it("deep-copies an array of a string and an integer", function()
    local sbuf = ffi.new("char[6]", "hello")
    local items = ffi.new("Object[2]")
    items[0].type = ffi.C.kObjectTypeString
    items[0].data.string.data = sbuf
    items[0].data.string.size = 5
    items[1].type = ffi.C.kObjectTypeInteger
    items[1].data.integer = 42
    local src = ffi.new("Object[1]")
    src[0].type = ffi.C.kObjectTypeArray
    src[0].data.array.items = items
    src[0].data.array.size = 2
    src[0].data.array.capacity = 2

    local dst = ffi.new("Object[1]")
    dst[0] = lib.api_copy_object(src)
    assert.equal(ffi.C.kObjectTypeArray, dst[0].type)
    assert.equal(2, tonumber(dst[0].data.array.size))
    local first = dst[0].data.array.items[0]
    assert.equal("hello", ffi.string(first.data.string.data, first.data.string.size))
    assert.is_true(first.data.string.data ~= sbuf) -- deep: distinct allocation
    assert.equal(42, tonumber(dst[0].data.array.items[1].data.integer))
    lib.api_free_object(dst)
    assert.equal(ffi.C.kObjectTypeNil, dst[0].type)
    lib.api_free_object(dst) -- idempotent
  end)

  it("deep-copies a dict, duplicating keys and values", function()
    local kbuf = ffi.new("char[2]", "a")
    local vbuf = ffi.new("char[2]", "x")
    local kvs = ffi.new("KeyValuePair[1]")
    kvs[0].key.data = kbuf
    kvs[0].key.size = 1
    kvs[0].value.type = ffi.C.kObjectTypeString
    kvs[0].value.data.string.data = vbuf
    kvs[0].value.data.string.size = 1
    local src = ffi.new("Object[1]")
    src[0].type = ffi.C.kObjectTypeDict
    src[0].data.dict.items = kvs
    src[0].data.dict.size = 1
    src[0].data.dict.capacity = 1

    local dst = ffi.new("Object[1]")
    dst[0] = lib.api_copy_object(src)
    assert.equal(ffi.C.kObjectTypeDict, dst[0].type)
    assert.equal(1, tonumber(dst[0].data.dict.size))
    local pair = dst[0].data.dict.items[0]
    assert.equal("a", ffi.string(pair.key.data, pair.key.size))
    assert.equal("x", ffi.string(pair.value.data.string.data, pair.value.data.string.size))
    assert.is_true(pair.key.data ~= kbuf) -- deep
    lib.api_free_object(dst)
    assert.equal(ffi.C.kObjectTypeNil, dst[0].type)
  end)

  it("walks nested containers (array of dict of array)", function()
    local inner = ffi.new("Object[2]")
    inner[0].type = ffi.C.kObjectTypeInteger
    inner[0].data.integer = 1
    inner[1].type = ffi.C.kObjectTypeInteger
    inner[1].data.integer = 2
    local kbuf = ffi.new("char[2]", "k")
    local kvs = ffi.new("KeyValuePair[1]")
    kvs[0].key.data = kbuf
    kvs[0].key.size = 1
    kvs[0].value.type = ffi.C.kObjectTypeArray
    kvs[0].value.data.array.items = inner
    kvs[0].value.data.array.size = 2
    kvs[0].value.data.array.capacity = 2
    local outer = ffi.new("Object[1]")
    outer[0].type = ffi.C.kObjectTypeDict
    outer[0].data.dict.items = kvs
    outer[0].data.dict.size = 1
    outer[0].data.dict.capacity = 1
    local src = ffi.new("Object[1]")
    src[0].type = ffi.C.kObjectTypeArray
    src[0].data.array.items = outer
    src[0].data.array.size = 1
    src[0].data.array.capacity = 1

    local dst = ffi.new("Object[1]")
    dst[0] = lib.api_copy_object(src)
    local nested = dst[0].data.array.items[0].data.dict.items[0].value
    assert.equal(ffi.C.kObjectTypeArray, nested.type)
    assert.equal(2, tonumber(nested.data.array.size))
    assert.equal(1, tonumber(nested.data.array.items[0].data.integer))
    assert.equal(2, tonumber(nested.data.array.items[1].data.integer))
    lib.api_free_object(dst) -- frees the whole nested tree (ASan validates)
  end)

  it("frees a Nil or zeroed object idempotently", function()
    local o = ffi.new("Object[1]") -- zero-initialized => kObjectTypeNil
    lib.api_free_object(o)
    assert.equal(ffi.C.kObjectTypeNil, o[0].type)
    lib.api_free_object(o) -- idempotent
  end)
end)
