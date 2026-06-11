local t = require("test.unit.helpers")
local ffi = t.ffi
local lib = t.lib

-- Declarations mirror src/mua/api/private/helpers.h (types come from helpers).
t.cdef([[
  void api_set_error(Error *err, ErrorType type, const char *fmt, ...);
  String cstr_to_string(const char *str);
  String cstr_as_string(const char *str);
  void api_free_string(String str);
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
