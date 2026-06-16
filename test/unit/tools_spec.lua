local t = require("test.unit.helpers")
local ffi = t.ffi
local lib = t.lib

-- Declarations mirror src/mua/tools.h (cJSON and json_* come from helpers).
-- Field order must match the C struct exactly: the FFI reads raw layout.
t.cdef([[
  typedef struct { char *content; bool is_error; } ToolResult;
  typedef struct ToolExec ToolExec;
  typedef void (*ToolDoneCb)(void *ud, const ToolResult *result);
  typedef struct ToolDef ToolDef;
  struct ToolDef {
    const char *name;
    const char *description;
    const char *params_schema;
    cJSON *schema_json;
    ToolExec *(*execute)(const ToolDef *def, cJSON *args, ToolDoneCb done, void *ud);
    bool mutating;
    int callback;
  };
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
  local exec = def.execute(def, args, cb, nil)
  cb:free()
  lib.json_free(args)
  assert.is_true(exec == nil) -- sync contract: done fired inline
  assert.equal(1, calls)
  return captured
end

-- Shared filesystem fixture for every tool describe below.
local root

local function write_file(name, data)
  local path = root .. "/" .. name
  local f = assert(io.open(path, "wb"))
  f:write(data)
  f:close()
  return path
end

local function read_back(path)
  local f = assert(io.open(path, "rb"))
  local data = f:read("*a")
  f:close()
  return data
end

before_each(function()
  root = os.tmpname()
  os.remove(root)
  assert(os.execute('mkdir -p "' .. root .. '"'))
end)

after_each(function()
  -- The unwritable-parent test leaves a 0500 dir; unlock before deleting.
  os.execute('chmod -R u+rwx "' .. root .. '" 2>/dev/null')
  os.execute('rm -rf "' .. root .. '"')
end)

describe("tools registry", function()
  it("looks up read with the right flags", function()
    local def = lib.tools_lookup("read")
    assert.is_true(def ~= nil)
    assert.equal("read", ffi.string(def.name))
    assert.is_false(def.mutating)
  end)

  it("write, edit, and bash are registered as mutating", function()
    for _, name in ipairs({ "write", "edit", "bash" }) do
      local def = lib.tools_lookup(name)
      assert.is_true(def ~= nil)
      assert.equal(name, ffi.string(def.name))
      assert.is_true(def.mutating)
    end
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
    assert.equal(4, select(2, s:gsub('"type":"function"', "")))
    assert.truthy(s:find('"name":"read"', 1, true))
    assert.truthy(s:find('"name":"write"', 1, true))
    assert.truthy(s:find('"name":"edit"', 1, true))
    assert.truthy(s:find('"name":"bash"', 1, true))
    assert.truthy(s:find('"required":["path"]', 1, true))
    assert.truthy(s:find('"required":["path","old_string","new_string"]', 1, true))
    lib.json_free(parse(s)) -- round-trip: the printed array re-parses
  end)
end)

describe("read tool", function()
  local def = lib.tools_lookup("read")

  local function read_args(path, extra)
    return '{"path":"' .. path .. '"' .. (extra or "") .. "}"
  end

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

describe("write tool", function()
  local def = lib.tools_lookup("write")

  local function write_args(path, content)
    return '{"path":"' .. path .. '","content":"' .. content .. '"}'
  end

  it("writes a file and reports the byte count", function()
    local path = root .. "/f.txt"
    local r = run_tool(def, write_args(path, "hello world"))
    assert.is_false(r.is_error)
    assert.equal("write: wrote 11 bytes to " .. path, r.content)
    assert.equal("hello world", read_back(path))
  end)

  it("creates nested parent directories", function()
    local path = root .. "/a/b/c/f.txt"
    local r = run_tool(def, write_args(path, "deep"))
    assert.is_false(r.is_error)
    assert.equal("deep", read_back(path))
  end)

  it("replaces existing contents entirely", function()
    local path = write_file("f.txt", "a much longer original body\n")
    local r = run_tool(def, write_args(path, "short"))
    assert.is_false(r.is_error)
    assert.equal("short", read_back(path))
  end)

  it("writes an empty file for empty content", function()
    local path = root .. "/f.txt"
    local r = run_tool(def, write_args(path, ""))
    assert.is_false(r.is_error)
    assert.equal("write: wrote 0 bytes to " .. path, r.content)
    assert.equal("", read_back(path))
  end)

  it("errors on a directory path", function()
    local r = run_tool(def, write_args(root, "x"))
    assert.is_true(r.is_error)
    assert.truthy(r.content:find("cannot open", 1, true))
  end)

  it("errors on an unwritable parent", function()
    assert(os.execute('mkdir -p "' .. root .. '/locked" && chmod 0500 "' .. root .. '/locked"'))
    local r = run_tool(def, write_args(root .. "/locked/f.txt", "x"))
    assert.is_true(r.is_error)
  end)

  it("requires string path and content", function()
    local no_content = run_tool(def, '{"path":"' .. root .. '/f.txt"}')
    assert.is_true(no_content.is_error)
    assert.truthy(no_content.content:find("content is required", 1, true))
    local bad_content = run_tool(def, '{"path":"' .. root .. '/f.txt","content":42}')
    assert.is_true(bad_content.is_error)
    local no_path = run_tool(def, '{"content":"x"}')
    assert.is_true(no_path.is_error)
  end)
end)

describe("edit tool", function()
  local def = lib.tools_lookup("edit")

  local function edit_args(path, old, new)
    return '{"path":"' .. path .. '","old_string":"' .. old .. '","new_string":"' .. new .. '"}'
  end

  it("replaces a unique occurrence and reports it", function()
    local path = write_file("f.txt", "before MARK after\n")
    local r = run_tool(def, edit_args(path, "MARK", "X"))
    assert.is_false(r.is_error)
    assert.equal("edit: replaced 1 occurrence in " .. path, r.content)
    assert.equal("before X after\n", read_back(path))
  end)

  it("errors when old_string is not found", function()
    local path = write_file("f.txt", "abc\n")
    local r = run_tool(def, edit_args(path, "zzz", "x"))
    assert.is_true(r.is_error)
    assert.truthy(r.content:find("not found", 1, true))
    assert.equal("abc\n", read_back(path))
  end)

  it("errors with the count on multiple occurrences", function()
    local path = write_file("f.txt", "dup one dup two dup\n")
    local r = run_tool(def, edit_args(path, "dup", "x"))
    assert.is_true(r.is_error)
    assert.truthy(r.content:find("occurs 3 times", 1, true))
    assert.equal("dup one dup two dup\n", read_back(path))
  end)

  it("counts overlapping matches and refuses them", function()
    local path = write_file("f.txt", "aaa")
    local r = run_tool(def, edit_args(path, "aa", "b"))
    assert.is_true(r.is_error)
    assert.truthy(r.content:find("occurs 2 times", 1, true))
  end)

  it("rejects empty and identical old/new strings", function()
    local path = write_file("f.txt", "abc\n")
    local empty_old = run_tool(def, edit_args(path, "", "x"))
    assert.is_true(empty_old.is_error)
    assert.truthy(empty_old.content:find("old_string is empty", 1, true))
    local same = run_tool(def, edit_args(path, "abc", "abc"))
    assert.is_true(same.is_error)
    assert.truthy(same.content:find("identical", 1, true))
    local missing_new = run_tool(def, '{"path":"' .. path .. '","old_string":"abc"}')
    assert.is_true(missing_new.is_error)
  end)

  it("deletes the match when new_string is empty", function()
    local path = write_file("f.txt", "keep CUT keep\n")
    local r = run_tool(def, edit_args(path, " CUT", ""))
    assert.is_false(r.is_error)
    assert.equal("keep keep\n", read_back(path))
  end)

  it("matches at byte 0, at EOF, and as the whole file", function()
    local head = write_file("h.txt", "OLD rest\n")
    assert.is_false(run_tool(def, edit_args(head, "OLD", "NEW")).is_error)
    assert.equal("NEW rest\n", read_back(head))
    local tail = write_file("t.txt", "rest OLD")
    assert.is_false(run_tool(def, edit_args(tail, "OLD", "NEW")).is_error)
    assert.equal("rest NEW", read_back(tail))
    local whole = write_file("w.txt", "OLD")
    assert.is_false(run_tool(def, edit_args(whole, "OLD", "brand new body")).is_error)
    assert.equal("brand new body", read_back(whole))
  end)

  it("grows and shrinks the file through the rewrite", function()
    local grow = write_file("g.txt", "a[X]b")
    assert.is_false(run_tool(def, edit_args(grow, "[X]", "[XXXXXXXX]")).is_error)
    assert.equal("a[XXXXXXXX]b", read_back(grow))
    local shrink = write_file("s.txt", "a[XXXXXXXX]b")
    assert.is_false(run_tool(def, edit_args(shrink, "[XXXXXXXX]", "[X]")).is_error)
    assert.equal("a[X]b", read_back(shrink))
  end)

  it("enforces the 4 MiB cap and leaves the file untouched", function()
    local body = string.rep("a", 4 * 1024 * 1024 + 1)
    local path = write_file("big.txt", body)
    local r = run_tool(def, edit_args(path, "aaa", "b"))
    assert.is_true(r.is_error)
    assert.truthy(r.content:find("4 MiB", 1, true))
    assert.equal(#body, #read_back(path))
  end)

  it("accepts a file of exactly 4 MiB", function()
    local body = string.rep("a", 4 * 1024 * 1024 - 3) .. "XYZ"
    local path = write_file("cap.txt", body)
    local r = run_tool(def, edit_args(path, "XYZ", "END"))
    assert.is_false(r.is_error)
    assert.equal(string.rep("a", 4 * 1024 * 1024 - 3) .. "END", read_back(path))
  end)

  it("rewrites a symlink's target in place", function()
    local target = write_file("target.txt", "follow MARK here\n")
    local link = root .. "/link.txt"
    assert(os.execute('ln -s "' .. target .. '" "' .. link .. '"'))
    local r = run_tool(def, edit_args(link, "MARK", "X"))
    assert.is_false(r.is_error)
    assert.equal("follow X here\n", read_back(target))
    local pipe = assert(io.popen('ls -l "' .. link .. '"'))
    local line = pipe:read("*l") or ""
    pipe:close()
    assert.truthy(line:match("^l")) -- still a symlink, not replaced by a file
  end)

  it("errors on a missing file", function()
    local r = run_tool(def, edit_args(root .. "/nope.txt", "a", "b"))
    assert.is_true(r.is_error)
    assert.truthy(r.content:find("cannot open", 1, true))
  end)
end)

describe("bash tool", function()
  local def = lib.tools_lookup("bash")

  setup(function()
    assert.is_true(lib.loop_init())
  end)

  teardown(function()
    assert.is_true(lib.loop_close()) -- fails loudly if bash leaked a handle
  end)

  --- Run bash to completion on the singleton loop. Returns the captured
  --- result and whether execute went async (returned a handle).
  local function run_bash(args_json)
    local args = parse(args_json)
    local captured
    local calls = 0
    local cb = ffi.cast("ToolDoneCb", function(_, result)
      calls = calls + 1
      captured = {
        content = result.content ~= nil and ffi.string(result.content) or nil,
        is_error = result.is_error,
      }
      lib.xfree(result.content)
    end)
    local exec = def.execute(def, args, cb, nil)
    lib.json_free(args) -- borrowed only for the call; uv_spawn copied argv
    local async = exec ~= nil
    if async then
      assert.equal(0, lib.loop_run()) -- drains all three handles
    end
    assert.equal(1, calls) -- done fired exactly once, sync or async
    cb:free()
    return captured, async
  end

  it("captures output from a clean exit", function()
    local r, async = run_bash('{"command":"echo hi"}')
    assert.is_true(async)
    assert.is_false(r.is_error)
    assert.equal("hi\n", r.content)
  end)

  it("annotates a nonzero exit code", function()
    local r = run_bash('{"command":"echo out; exit 3"}')
    assert.is_true(r.is_error)
    assert.equal("out\n\n[bash: exit code 3]", r.content)
  end)

  it("interleaves stdout and stderr in order", function()
    local r = run_bash('{"command":"echo one; echo two >&2; echo three"}')
    assert.is_false(r.is_error)
    assert.equal("one\ntwo\nthree\n", r.content)
  end)

  it("kills a command at the timeout", function()
    local r = run_bash('{"command":"sleep 5","timeout_ms":100}')
    assert.is_true(r.is_error)
    assert.equal("[bash: timed out after 100 ms (SIGKILL)]", r.content)
  end)

  it("truncates at 64 KiB and drains to EOF", function()
    -- 3000 x 41-byte lines = ~123 KiB; a deadlocked drain would instead ride
    -- the default 30 s timeout and fail the suite on wall clock.
    local line = "0123456789012345678901234567890123456789"
    local r = run_bash('{"command":"yes ' .. line .. ' | head -n 3000"}')
    assert.is_false(r.is_error) -- head exits 0; truncation is not failure
    local note = "\n[bash: output truncated at 64 KiB]"
    assert.equal(64 * 1024 + #note, #r.content)
    assert.equal(line .. "\n", r.content:sub(1, 41))
    assert.truthy(r.content:find(note, 1, true))
  end)

  it("reports death by signal", function()
    local r = run_bash('{"command":"kill -TERM $$"}')
    assert.is_true(r.is_error)
    assert.equal("[bash: killed by signal 15]", r.content)
  end)

  it("gives up on orphaned pipe holders after the grace window", function()
    local r = run_bash('{"command":"echo hi; sleep 2 &"}')
    assert.is_false(r.is_error) -- the shell itself exited 0
    assert.equal(
      "hi\n\n[bash: stopped reading 500 ms after exit; "
        .. "a background process still holds the output pipe]",
      r.content
    )
  end)

  it("wires stdin to /dev/null", function()
    local r = run_bash('{"command":"read x && echo got || echo eof"}')
    assert.equal("eof\n", r.content)
  end)

  it("returns empty content for silent success", function()
    local r = run_bash('{"command":"true"}')
    assert.is_false(r.is_error)
    assert.equal("", r.content)
  end)

  it("validates command and timeout_ms inline", function()
    local no_command, async = run_bash("{}")
    assert.is_false(async) -- validation fails before any handle exists
    assert.is_true(no_command.is_error)
    assert.truthy(no_command.content:find("command is required", 1, true))
    local bad_timeout, async2 = run_bash('{"command":"true","timeout_ms":"x"}')
    assert.is_false(async2)
    assert.is_true(bad_timeout.is_error)
    assert.truthy(bad_timeout.content:find("timeout_ms", 1, true))
  end)

  it("cancel kills the command and still fires done", function()
    local args = parse('{"command":"sleep 5"}')
    local captured
    local calls = 0
    local cb = ffi.cast("ToolDoneCb", function(_, result)
      calls = calls + 1
      captured = { content = ffi.string(result.content), is_error = result.is_error }
      lib.xfree(result.content)
    end)
    local exec = def.execute(def, args, cb, nil)
    lib.json_free(args)
    assert.is_true(exec ~= nil)
    lib.tools_cancel(exec)
    assert.equal(0, lib.loop_run())
    assert.equal(1, calls)
    cb:free()
    assert.is_true(captured.is_error)
    assert.equal("[bash: canceled]", captured.content)
  end)
end)
