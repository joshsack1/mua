local t = require("test.unit.helpers")
local ffi = t.ffi
local lib = t.lib

-- Declarations mirror src/mua/paths.h (setenv/unsetenv come from libc).
t.cdef([[
  char *paths_config_dir(void);
  char *paths_state_dir(void);
  char *paths_runtime_dir(void);
  char *paths_join(const char *a, const char *b);
  int setenv(const char *name, const char *value, int overwrite);
  int unsetenv(const char *name);
]])

local function take(cstr)
  if cstr == nil then
    return nil
  end
  local s = ffi.string(cstr)
  lib.xfree(cstr)
  return s
end

describe("paths", function()
  local saved = {}
  local vars = { "MUA_CONFIG_DIR", "XDG_CONFIG_HOME", "MUA_RUNTIME" }

  before_each(function()
    for _, name in ipairs(vars) do
      saved[name] = os.getenv(name)
      assert.equal(0, ffi.C.unsetenv(name))
    end
  end)

  after_each(function()
    for _, name in ipairs(vars) do
      if saved[name] then
        assert.equal(0, ffi.C.setenv(name, saved[name], 1))
      else
        assert.equal(0, ffi.C.unsetenv(name))
      end
    end
  end)

  it("MUA_CONFIG_DIR override wins", function()
    assert.equal(0, ffi.C.setenv("MUA_CONFIG_DIR", "/tmp/custom-mua", 1))
    assert.equal("/tmp/custom-mua", take(lib.paths_config_dir()))
  end)

  it("XDG_CONFIG_HOME is used when no override", function()
    assert.equal(0, ffi.C.setenv("XDG_CONFIG_HOME", "/tmp/xdg", 1))
    assert.equal("/tmp/xdg/mua", take(lib.paths_config_dir()))
  end)

  it("falls back to ~/.config/mua", function()
    local home = assert(os.getenv("HOME"))
    assert.equal(home .. "/.config/mua", take(lib.paths_config_dir()))
  end)

  it("empty override is treated as unset", function()
    assert.equal(0, ffi.C.setenv("MUA_CONFIG_DIR", "", 1))
    local home = assert(os.getenv("HOME"))
    assert.equal(home .. "/.config/mua", take(lib.paths_config_dir()))
  end)

  it("runtime dir prefers MUA_RUNTIME and has a compiled default", function()
    assert.equal(0, ffi.C.setenv("MUA_RUNTIME", "/tmp/rt", 1))
    assert.equal("/tmp/rt", take(lib.paths_runtime_dir()))
    assert.equal(0, ffi.C.unsetenv("MUA_RUNTIME"))
    local compiled = take(lib.paths_runtime_dir())
    assert.truthy(compiled:match("/runtime$"))
  end)

  it("paths_join collapses trailing slashes", function()
    assert.equal("/a/b", take(lib.paths_join("/a/", "b")))
    assert.equal("/a/b/c", take(lib.paths_join("/a", "b/c")))
    assert.equal("/x", take(lib.paths_join("/", "x")))
  end)
end)
