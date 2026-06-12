local t = require("test.unit.helpers")
local ffi = t.ffi
local lib = t.lib

-- Declarations mirror src/mua/tools.h (cJSON and json_* come from helpers).
-- Field order must match the C struct exactly: the FFI reads raw layout.
t.cdef([[
  typedef struct { char *content; bool is_error; } ToolResult;
  typedef struct ToolExec ToolExec;
  typedef void (*ToolDoneCb)(void *ud, const ToolResult *result);
  typedef struct {
    const char *name;
    const char *description;
    const char *params_schema;
    bool mutating;
    ToolExec *(*execute)(cJSON *args, ToolDoneCb done, void *ud);
  } ToolDef;
  const ToolDef *tools_lookup(const char *name);
  cJSON *tools_build_openai_array(Error *err);
  void tools_cancel(ToolExec *exec);
]])

lib.json_init()

local BIG = 16 * 1024 * 1024

local anchors = {}

local function S(str)
  anchors[#anchors + 1] = str
  return ffi.new("String", ffi.cast("char *", str), #str)
end

local function new_error()
  local err = ffi.new("Error")
  err.type = ffi.C.kErrorTypeNone
  return err
end

local function parse(text)
  local perr = new_error()
  local node = lib.json_parse(S(text), BIG, perr)
  assert.equal(ffi.C.kErrorTypeNone, perr.type)
  assert.is_true(node ~= nil)
  return node
end

local function printed(node)
  local s = lib.json_print(node)
  local out = ffi.string(s.data, s.size)
  lib.xfree(s.data)
  return out
end

--- Run a tool's execute synchronously, capturing the single done result.
local function run_tool(def, args_json)
  local args = parse(args_json)
  local captured
  local calls = 0
  local cb = ffi.cast("ToolDoneCb", function(_, result)
    calls = calls + 1
    captured = {
      content = result.content ~= nil and ffi.string(result.content) or nil,
      is_error = result.is_error,
    }
    lib.xfree(result.content) -- ownership of content transfers to the callee
  end)
  local exec = def.execute(args, cb, nil)
  cb:free()
  lib.json_free(args)
  assert.is_true(exec == nil) -- sync contract: done fired inline
  assert.equal(1, calls)
  return captured
end

describe("tools registry", function()
  it("looks up read with the right flags", function()
    local def = lib.tools_lookup("read")
    assert.is_true(def ~= nil)
    assert.equal("read", ffi.string(def.name))
    assert.is_false(def.mutating)
  end)

  it("returns NULL for unknown or NULL names", function()
    assert.is_true(lib.tools_lookup("no-such-tool") == nil)
    assert.is_true(lib.tools_lookup(nil) == nil)
  end)

  it("builds the wire-shaped openai tools array", function()
    local err = new_error()
    local arr = lib.tools_build_openai_array(err)
    assert.is_true(arr ~= nil)
    assert.equal(ffi.C.kErrorTypeNone, err.type)
    local s = printed(arr)
    lib.json_free(arr)
    assert.truthy(s:find('"type":"function"', 1, true))
    assert.truthy(s:find('"name":"read"', 1, true))
    assert.truthy(s:find('"required":["path"]', 1, true))
    lib.json_free(parse(s)) -- round-trip: the printed array re-parses
  end)
end)

describe("read tool", function()
  local def = lib.tools_lookup("read")
  local root

  local function write_file(name, data)
    local path = root .. "/" .. name
    local f = assert(io.open(path, "wb"))
    f:write(data)
    f:close()
    return path
  end

  local function read_args(path, extra)
    return '{"path":"' .. path .. '"' .. (extra or "") .. "}"
  end

  before_each(function()
    root = os.tmpname()
    os.remove(root)
    assert(os.execute('mkdir -p "' .. root .. '"'))
  end)

  after_each(function()
    os.execute('rm -rf "' .. root .. '"')
  end)

  it("returns a whole file verbatim", function()
    local path = write_file("f.txt", "l1\nl2\nl3\n")
    local r = run_tool(def, read_args(path))
    assert.is_false(r.is_error)
    assert.equal("l1\nl2\nl3\n", r.content)
  end)

  it("keeps a missing trailing newline", function()
    local path = write_file("f.txt", "a\nb")
    local r = run_tool(def, read_args(path))
    assert.is_false(r.is_error)
    assert.equal("a\nb", r.content)
  end)

  it("returns an empty file as empty content", function()
    local path = write_file("f.txt", "")
    local r = run_tool(def, read_args(path))
    assert.is_false(r.is_error)
    assert.equal("", r.content)
  end)

  it("windows by offset and limit (1-based lines)", function()
    local lines = {}
    for i = 1, 10 do
      lines[#lines + 1] = "l" .. i
    end
    local path = write_file("f.txt", table.concat(lines, "\n") .. "\n")
    assert.equal(
      "l3\nl4\nl5\nl6\nl7\nl8\nl9\nl10\n",
      run_tool(def, read_args(path, ',"offset":3')).content
    )
    assert.equal("l1\nl2\n", run_tool(def, read_args(path, ',"limit":2')).content)
    assert.equal("l3\nl4\n", run_tool(def, read_args(path, ',"offset":3,"limit":2')).content)
  end)

  it("notes an offset past the end of file without erroring", function()
    local path = write_file("f.txt", "a\nb\n")
    local r = run_tool(def, read_args(path, ',"offset":99'))
    assert.is_false(r.is_error)
    assert.equal("[read: offset past end of file]", r.content)
  end)

  it("head-truncates at 256 KiB with an annotation", function()
    local cap = 256 * 1024
    local path = write_file("big.txt", string.rep("x", 300 * 1024))
    local r = run_tool(def, read_args(path))
    assert.is_false(r.is_error)
    local note = "\n[read: truncated at 256 KiB]"
    assert.equal(cap + #note, #r.content)
    assert.equal(string.rep("x", cap) .. note, r.content)
  end)

  it("stops at the 8 MiB scan bound", function()
    -- One 9 MiB line: line 2 is never reached within the scan bound.
    local path = write_file("huge.txt", string.rep("z", 9 * 1024 * 1024))
    local r = run_tool(def, read_args(path, ',"offset":2'))
    assert.is_false(r.is_error)
    assert.equal("[read: stopped at the 8 MiB scan bound]", r.content)
  end)

  it("rejects binary files", function()
    local path = write_file("bin.dat", "ab\0cd")
    local r = run_tool(def, read_args(path))
    assert.is_true(r.is_error)
    assert.truthy(r.content:find("binary", 1, true))
  end)

  it("rejects directories", function()
    local r = run_tool(def, read_args(root))
    assert.is_true(r.is_error)
    assert.truthy(r.content:find("not a regular file", 1, true))
  end)

  it("reports a missing file", function()
    local r = run_tool(def, read_args(root .. "/nope.txt"))
    assert.is_true(r.is_error)
    assert.truthy(r.content:find("cannot open", 1, true))
  end)

  it("validates its arguments", function()
    local no_path = run_tool(def, "{}")
    assert.is_true(no_path.is_error)
    assert.truthy(no_path.content:find("path is required", 1, true))
    local bad_path = run_tool(def, '{"path":42}')
    assert.is_true(bad_path.is_error)
    local path = write_file("f.txt", "a\n")
    local zero_offset = run_tool(def, read_args(path, ',"offset":0'))
    assert.is_true(zero_offset.is_error)
    assert.truthy(zero_offset.content:find("integers >= 1", 1, true))
    local bad_limit = run_tool(def, read_args(path, ',"limit":"x"'))
    assert.is_true(bad_limit.is_error)
  end)
end)
