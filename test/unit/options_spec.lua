local t = require("test.unit.helpers")
local ffi = t.ffi
local lib = t.lib

-- Object/ObjectType mirror src/mua/api/private/defs.h (String/Error come from
-- the shared helpers cdef). The store is a process-global singleton and busted
-- runs one process, so every case resets it via options_free in before_each.
t.cdef([[
  typedef enum {
    kObjectTypeNil = 0, kObjectTypeBoolean, kObjectTypeInteger, kObjectTypeFloat,
    kObjectTypeString, kObjectTypeArray, kObjectTypeDict, kObjectTypeSession
  } ObjectType;
  typedef struct object Object;
  typedef struct key_value_pair KeyValuePair;
  typedef struct { Object *items; size_t size; size_t capacity; } Array;
  typedef struct { KeyValuePair *items; size_t size; size_t capacity; } Dict;
  struct object {
    ObjectType type;
    union { bool boolean; int64_t integer; double floating; String string; Array array; Dict dict; } data;
  };
  struct key_value_pair { String key; Object value; };

  void api_free_string(String str);
  void options_set(String name, Object value, Error *err);
  Object options_get(String name, Error *err);
  const char *options_system_prompt(void);
  const char *options_model_borrow(void);
  int options_step_cap(void);
  void options_free(void);
]])

local function new_error()
  local err = ffi.new("Error")
  err.type = ffi.C.kErrorTypeNone
  return err
end

-- Build name (and a string value) inside the calling helper so the char[]
-- buffers stay anchored across the C call; the store copies, so they may die
-- after. Returning a String that pointed into a freed local would dangle.
local function set_str(name, value)
  local nbuf = ffi.new("char[?]", #name + 1, name)
  local vbuf = ffi.new("char[?]", #value + 1, value)
  local nm = ffi.new("String", { data = nbuf, size = #name })
  local o = ffi.new("Object")
  o.type = ffi.C.kObjectTypeString
  o.data.string.data = vbuf
  o.data.string.size = #value
  local err = new_error()
  lib.options_set(nm, o, err)
  return err
end

local function set_int(name, n)
  local nbuf = ffi.new("char[?]", #name + 1, name)
  local nm = ffi.new("String", { data = nbuf, size = #name })
  local o = ffi.new("Object")
  o.type = ffi.C.kObjectTypeInteger
  o.data.integer = n
  local err = new_error()
  lib.options_set(nm, o, err)
  return err
end

local function get(name)
  local nbuf = ffi.new("char[?]", #name + 1, name)
  local nm = ffi.new("String", { data = nbuf, size = #name })
  local err = new_error()
  local o = lib.options_get(nm, err)
  return o, err
end

local function is_validation(err)
  return err.type == ffi.C.kErrorTypeValidation
end

describe("options store", function()
  before_each(function()
    lib.options_free() -- isolate cases: one process, one shared singleton
  end)

  describe("step_cap", function()
    it("round-trips a valid value through the typed getter and options_get", function()
      local err = set_int("step_cap", 30)
      assert.equal(ffi.C.kErrorTypeNone, err.type)
      assert.equal(30, lib.options_step_cap())
      local o = get("step_cap")
      assert.equal(ffi.C.kObjectTypeInteger, o.type)
      assert.equal(30, tonumber(o.data.integer))
    end)

    it("defaults to the cap maximum when unset", function()
      assert.equal(50, lib.options_step_cap())
      local o = get("step_cap")
      assert.equal(50, tonumber(o.data.integer))
    end)

    it("rejects out-of-range values and leaves the store unchanged", function()
      for _, bad in ipairs({ 0, -5, 51, 999 }) do
        local err = set_int("step_cap", bad)
        assert.is_true(is_validation(err), "expected validation error for " .. bad)
        assert.truthy(ffi.string(err.msg):find("step_cap", 1, true))
        lib.api_clear_error(err)
        assert.equal(50, lib.options_step_cap()) -- still the default
      end
    end)

    it("rejects a string assigned to an integer option (type mismatch)", function()
      local err = set_str("step_cap", "nope")
      assert.is_true(is_validation(err))
      assert.truthy(ffi.string(err.msg):find("integer", 1, true))
      lib.api_clear_error(err)
    end)
  end)

  describe("model", function()
    it("round-trips and borrows the set string", function()
      set_str("model", "z-ai/glm-5.1")
      assert.equal("z-ai/glm-5.1", ffi.string(lib.options_model_borrow()))
    end)

    it("borrows NULL and gets Nil when unset (provider default applies)", function()
      assert.is_true(lib.options_model_borrow() == nil)
      local o = get("model")
      assert.equal(ffi.C.kObjectTypeNil, o.type)
    end)
  end)

  describe("system_prompt (tri-state)", function()
    it("returns the built-in default when unset", function()
      local p = ffi.string(lib.options_system_prompt())
      assert.truthy(p:find("minimal coding agent", 1, true))
      local o = get("system_prompt")
      assert.equal(ffi.C.kObjectTypeString, o.type)
      lib.api_free_string(o.data.string)
    end)

    it("returns the empty string when set to '' (omit the system message)", function()
      set_str("system_prompt", "")
      assert.equal("", ffi.string(lib.options_system_prompt()))
    end)

    it("returns the set value", function()
      set_str("system_prompt", "You are X")
      assert.equal("You are X", ffi.string(lib.options_system_prompt()))
    end)

    it("overwrites a prior value", function()
      set_str("system_prompt", "first")
      set_str("system_prompt", "second")
      assert.equal("second", ffi.string(lib.options_system_prompt()))
    end)
  end)

  describe("unknown options", function()
    it("set names the offending key and leaves the store untouched", function()
      local err = set_int("bogus", 1)
      assert.is_true(is_validation(err))
      assert.truthy(ffi.string(err.msg):find("bogus", 1, true))
      lib.api_clear_error(err)
    end)

    it("get errors and returns Nil", function()
      local o, err = get("bogus")
      assert.is_true(is_validation(err))
      assert.equal(ffi.C.kObjectTypeNil, o.type)
      lib.api_clear_error(err)
    end)
  end)

  it("options_get returns owned copies safe to free independently", function()
    set_str("model", "owned/copy")
    local a = get("model")
    local b = get("model")
    assert.equal("owned/copy", ffi.string(a.data.string.data, a.data.string.size))
    lib.api_free_string(a.data.string)
    lib.api_free_string(b.data.string) -- distinct allocations; ASan would catch a double free
    assert.equal("owned/copy", ffi.string(lib.options_model_borrow())) -- store master intact
  end)
end)
