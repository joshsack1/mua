local t = require("test.unit.helpers")
local ffi = t.ffi
local lib = t.lib

-- Buf is typedef'd by memory_spec too; specs share one LuaJIT process and a
-- duplicate typedef aborts a cdef block midway. Keep Buf in its own block (a
-- redefine there is harmless -- it is already defined) so the Renderer
-- declarations below, which reference Buf, are never dropped.
t.cdef([[
  typedef struct { char *data; size_t size; size_t cap; size_t max; } Buf;
  void buf_init(Buf *buf, size_t max);
  void buf_free(Buf *buf);
]])
t.cdef([[
  typedef struct {
    Buf line;
    bool in_fence;
    bool line_overflowed;
    bool ended_newline;
  } Renderer;
  void render_init(Renderer *r);
  bool render_feed(Renderer *r, String in, Buf *out);
  bool render_flush(Renderer *r, Buf *out);
  void render_free(Renderer *r);
]])

local ESC = "\27"
local BOLD_ON, BOLD_OFF = ESC .. "[1m", ESC .. "[22m"
local IT_ON, IT_OFF = ESC .. "[3m", ESC .. "[23m"
local CODE_ON, CODE_OFF = ESC .. "[36m", ESC .. "[39m"
local DIM_ON, DIM_OFF = ESC .. "[2m", ESC .. "[22m"

-- Feeds the chunks through one renderer (big out buffer, never drained, so it
-- accumulates everything), flushes, and returns the styled output plus the
-- final ended_newline flag.
local function render(chunks)
  local r = ffi.new("Renderer")
  local out = ffi.new("Buf")
  lib.render_init(r)
  lib.buf_init(out, 1024 * 1024)
  local in_str = ffi.new("String")
  for _, chunk in ipairs(chunks) do
    in_str.data = ffi.cast("char *", chunk)
    in_str.size = #chunk
    assert.is_true(lib.render_feed(r, in_str, out))
  end
  assert.is_true(lib.render_flush(r, out))
  local result = ffi.string(out.data, out.size)
  local ended = r.ended_newline
  lib.render_free(r)
  lib.buf_free(out)
  return result, ended
end

describe("render: plain text", function()
  it("passes a plain line through unchanged and ends on the newline", function()
    local out, ended = render({ "hello world\n" })
    assert.equal("hello world\n", out)
    assert.is_true(ended)
  end)

  it("flushes a trailing partial line and reports no ending newline", function()
    local out, ended = render({ "hello" })
    assert.equal("hello", out)
    assert.is_false(ended)
  end)

  it("preserves multibyte UTF-8 bytes verbatim", function()
    assert.equal("caf\xc3\xa9\n", (render({ "caf\xc3\xa9\n" })))
  end)

  it("treats an empty chunk as a no-op", function()
    assert.equal("", (render({ "" })))
  end)
end)

describe("render: inline styling", function()
  it("styles **bold**", function()
    assert.equal("a " .. BOLD_ON .. "b" .. BOLD_OFF .. " c\n", (render({ "a **b** c\n" })))
  end)

  it("styles *italic*", function()
    assert.equal("a " .. IT_ON .. "b" .. IT_OFF .. " c\n", (render({ "a *b* c\n" })))
  end)

  it("styles `inline code`", function()
    assert.equal("a " .. CODE_ON .. "b" .. CODE_OFF .. " c\n", (render({ "a `b` c\n" })))
  end)

  it("emits an unmatched marker literally", function()
    assert.equal("a ** b\n", (render({ "a ** b\n" })))
  end)

  it("honors a backslash escape", function()
    assert.equal("a * b\n", (render({ "a \\* b\n" })))
  end)

  it("leaves underscores literal (no intraword-emphasis footgun)", function()
    assert.equal("my_var and __init__\n", (render({ "my_var and __init__\n" })))
  end)

  it("styles a marker split across two feed chunks", function()
    assert.equal(
      "a " .. BOLD_ON .. "bold" .. BOLD_OFF .. " c\n",
      (render({ "a **bo", "ld** c\n" }))
    )
  end)
end)

describe("render: headings", function()
  it("bolds an ATX heading and strips the hashes", function()
    assert.equal(BOLD_ON .. "Title" .. BOLD_OFF .. "\n", (render({ "## Title\n" })))
  end)

  it("does not re-scan a heading's content for inline markers", function()
    assert.equal(BOLD_ON .. "**x**" .. BOLD_OFF .. "\n", (render({ "# **x**\n" })))
  end)

  it("renders 7+ hashes literally (not a heading)", function()
    assert.equal("####### nope\n", (render({ "####### nope\n" })))
  end)
end)

describe("render: lists and blockquotes", function()
  it("passes a bullet marker through without treating it as emphasis", function()
    assert.equal("* item\n", (render({ "* item\n" })))
  end)

  it("does not italicize a bullet line with a trailing asterisk", function()
    assert.equal("* a *\n", (render({ "* a *\n" })))
  end)

  it("styles emphasis in a list item's content", function()
    assert.equal("- " .. BOLD_ON .. "x" .. BOLD_OFF .. "\n", (render({ "- **x**\n" })))
  end)
end)

describe("render: fenced code blocks", function()
  it("dims the delimiters and colors the body, with no inline scan inside", function()
    local out = render({ "```\n**x**\n```\n" })
    assert.equal(
      DIM_ON
        .. "```"
        .. DIM_OFF
        .. "\n"
        .. CODE_ON
        .. "**x**"
        .. CODE_OFF
        .. "\n"
        .. DIM_ON
        .. "```"
        .. DIM_OFF
        .. "\n",
      out
    )
  end)

  it("closes styling per-line so an unterminated fence leaves nothing open", function()
    local out, ended = render({ "```\ncode" })
    assert.equal(DIM_ON .. "```" .. DIM_OFF .. "\n" .. CODE_ON .. "code" .. CODE_OFF, out)
    assert.is_false(ended)
  end)
end)

describe("render: bounds and failure", function()
  it("returns false when the out buffer is exhausted mid-line", function()
    local r = ffi.new("Renderer")
    local out = ffi.new("Buf")
    lib.render_init(r)
    lib.buf_init(out, 3) -- too small to hold a styled line
    local in_str = ffi.new("String")
    local s = "hello\n"
    in_str.data = ffi.cast("char *", s)
    in_str.size = #s
    assert.is_false(lib.render_feed(r, in_str, out))
    lib.render_free(r)
    lib.buf_free(out)
  end)
end)
