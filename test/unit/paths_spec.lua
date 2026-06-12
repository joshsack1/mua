local t = require("test.unit.helpers")
local ffi = t.ffi
local lib = t.lib

-- Declarations mirror src/mua/paths.h (setenv/unsetenv come from libc).
t.cdef([[
  char *paths_config_dir(void);
  char *paths_state_dir(void);
  char *paths_runtime_dir(void);
  char *paths_join(const char *a, const char *b);
  bool paths_ensure_dir(const char *path, Error *err);
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

describe("paths_ensure_dir", function()
  local root
  local err

  -- "drwx------" from `ls -ld`: directory-ness and mode 0700 in one portable
  -- check (macOS appends @/+ after the 10-char mode field; prefix unaffected).
  local function is_dir_0700(path)
    local pipe = assert(io.popen('ls -ld "' .. path .. '" 2>/dev/null'))
    local line = pipe:read("*l") or ""
    pipe:close()
    return line:match("^drwx%-%-%-%-%-%-") ~= nil
  end

  before_each(function()
    root = os.tmpname()
    os.remove(root) -- want the free name, not the file tmpname may create
    err = ffi.new("Error")
    err.type = ffi.C.kErrorTypeNone
  end)

  after_each(function()
    lib.api_clear_error(err)
    os.execute('rm -rf "' .. root .. '"')
  end)

  it("creates nested directories with mode 0700", function()
    assert.is_true(lib.paths_ensure_dir(root .. "/a/b/c", err))
    assert.equal(ffi.C.kErrorTypeNone, err.type)
    assert.is_true(is_dir_0700(root .. "/a/b/c"))
    assert.is_true(is_dir_0700(root .. "/a"))
  end)

  it("is idempotent", function()
    assert.is_true(lib.paths_ensure_dir(root .. "/a/b", err))
    assert.is_true(lib.paths_ensure_dir(root .. "/a/b", err))
    assert.equal(ffi.C.kErrorTypeNone, err.type)
  end)

  it("errors when a file is in the way at the leaf", function()
    assert.is_true(lib.paths_ensure_dir(root, err))
    assert(io.open(root .. "/f", "w")):close()
    assert.is_false(lib.paths_ensure_dir(root .. "/f", err))
    assert.equal(ffi.C.kErrorTypeValidation, err.type)
    assert.is_true(err.msg ~= nil)
  end)

  it("errors when a file is in the way mid-path", function()
    assert.is_true(lib.paths_ensure_dir(root, err))
    assert(io.open(root .. "/f", "w")):close()
    assert.is_false(lib.paths_ensure_dir(root .. "/f/sub", err))
    assert.equal(ffi.C.kErrorTypeValidation, err.type)
  end)

  it("rejects an empty path", function()
    assert.is_false(lib.paths_ensure_dir("", err))
    assert.equal(ffi.C.kErrorTypeValidation, err.type)
  end)
end)
