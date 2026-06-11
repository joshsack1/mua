local t = require("test.unit.helpers")
local ffi = t.ffi
local lib = t.lib
local bit = require("bit")

-- Declarations mirror src/mua/sse.h (shared types come from helpers).
t.cdef([[
  typedef struct SseParser SseParser;
  typedef struct {
    size_t max_line; size_t max_event_data; size_t max_event_type; size_t max_id;
  } SseLimits;
  typedef bool (*SseEventCb)(void *ud, const String *event_type, const String *data,
                             const String *id);
  SseParser *sse_parser_new(const SseLimits *limits, SseEventCb cb, void *ud);
  void sse_parser_free(SseParser *parser);
  bool sse_parser_feed(SseParser *parser, const char *bytes, size_t len, Error *err);
  typedef enum { kSseEofClean = 0, kSseEofTruncated } SseEof;
  SseEof sse_parser_finish(const SseParser *parser);
  void sse_parser_reset(SseParser *parser);
]])

local function view(s)
  return s.size == 0 and "" or ffi.string(s.data, s.size)
end

--- Feed `chunks` through a fresh parser; capture the complete verdict tuple.
---@param chunks table list of byte strings
---@param limits ffi.cdata*|nil SseLimits
---@param abort_after number|nil return false from the callback after N events
local function run_stream(chunks, limits, abort_after)
  local events = {}
  local cb = ffi.cast("SseEventCb", function(_, etype, data, id)
    events[#events + 1] = { type = view(etype), data = view(data), id = view(id) }
    if abort_after and #events >= abort_after then
      return false
    end
    return true
  end)
  local parser = lib.sse_parser_new(limits, cb, nil)
  local err = ffi.new("Error")
  err.type = ffi.C.kErrorTypeNone
  local ok = true
  for _, chunk in ipairs(chunks) do
    ok = lib.sse_parser_feed(parser, chunk, #chunk, err)
    if not ok then
      break
    end
  end
  local errmsg = err.msg ~= nil and ffi.string(err.msg) or nil
  local truncated = lib.sse_parser_finish(parser) == ffi.C.kSseEofTruncated
  lib.sse_parser_free(parser)
  lib.api_clear_error(err)
  cb:free()
  return { events = events, ok = ok, errmsg = errmsg, truncated = truncated }
end

local function explode(s)
  local out = {}
  for i = 1, #s do
    out[i] = s:sub(i, i)
  end
  return out
end

local function make_prng(seed)
  local state = ffi.new("uint64_t[1]", seed)
  return function(n) -- uniform-ish in 1..n
    local x = state[0]
    x = bit.bxor(x, bit.lshift(x, 13))
    x = bit.bxor(x, bit.rshift(x, 7))
    x = bit.bxor(x, bit.lshift(x, 17))
    state[0] = x
    return tonumber(x % n) + 1
  end
end

local function random_splits(s, prng)
  local cuts = {}
  for _ = 1, prng(16) do
    cuts[#cuts + 1] = prng(#s - 1)
  end
  table.sort(cuts)
  local chunks, prev = {}, 0
  for _, cut in ipairs(cuts) do
    if cut > prev then
      chunks[#chunks + 1] = s:sub(prev + 1, cut)
      prev = cut
    end
  end
  chunks[#chunks + 1] = s:sub(prev + 1)
  return chunks
end

--- The core property: the verdict tuple is identical no matter how the
--- stream is chunked — whole-buffer, byte-by-byte, every two-chunk split,
--- and 200 fixed-seed random multi-splits.
local function assert_split_invariant(stream)
  local ref = run_stream({ stream })
  assert.same(ref, run_stream(explode(stream)), "byte-by-byte feed differs")
  for i = 1, #stream - 1 do
    local got = run_stream({ stream:sub(1, i), stream:sub(i + 1) })
    assert.same(ref, got, "two-chunk split at byte " .. i .. " differs")
  end
  local prng = make_prng(0xC0FFEEDEADBEEFULL)
  for trial = 1, 200 do
    local got = run_stream(random_splits(stream, prng))
    assert.same(ref, got, "random multi-split trial " .. trial .. " differs")
  end
  return ref
end

local OR_EVENTS = {
  'data: {"id":"gen-1","choices":[{"delta":{"content":"Hel"},"finish_reason":null}]}',
  "",
  'data: {"choices":[{"delta":{"content":"lo "}}]}',
  "",
  'data: {"choices":[{"delta":{"content":"world"},"finish_reason":"stop"}]}',
  "",
  "data: [DONE]",
  "",
}

local function join(lines, terminator)
  return table.concat(lines, terminator) .. terminator
end

describe("sse split invariance", function()
  it("OpenRouter-shaped stream, LF terminators (incl. mid-[DONE] splits)", function()
    local ref = assert_split_invariant(join(OR_EVENTS, "\n"))
    assert.equal(4, #ref.events)
    assert.is_true(ref.ok)
    assert.is_false(ref.truncated)
    assert.equal("message", ref.events[1].type)
    assert.truthy(ref.events[1].data:match('"Hel"'))
    assert.equal("[DONE]", ref.events[4].data)
  end)

  it("same stream, CRLF terminators", function()
    local ref = assert_split_invariant(join(OR_EVENTS, "\r\n"))
    assert.equal(4, #ref.events)
    assert.is_false(ref.truncated)
  end)

  it("same stream, bare CR terminators", function()
    local ref = assert_split_invariant(join(OR_EVENTS, "\r"))
    assert.equal(4, #ref.events)
    assert.is_false(ref.truncated)
  end)

  it("CRLF split exactly between CR and LF emits no phantom blank line", function()
    local stream = "data: a\r\n\r\ndata: b\r\n\r\n"
    local whole = run_stream({ stream })
    assert.equal(2, #whole.events)
    -- Cut between the '\r' and '\n' of the first terminator.
    local cut = ("data: a\r"):len()
    local split = run_stream({ stream:sub(1, cut), stream:sub(cut + 1) })
    assert.same(whole, split)
  end)

  it("mixed terminators in one stream", function()
    local stream = 'event: message_start\r\ndata: {"type":"message_start"}\n\r'
      .. "data: tail\rdata: more\n\n"
    local ref = assert_split_invariant(stream)
    assert.equal(2, #ref.events)
    assert.equal("message_start", ref.events[1].type)
    assert.equal("tail\nmore", ref.events[2].data)
  end)

  it("multi-line data joins with newline", function()
    local ref = assert_split_invariant("data: line1\ndata: line2\n\n")
    assert.equal(1, #ref.events)
    assert.equal("line1\nline2", ref.events[1].data)
  end)

  it("comments (incl. OPENROUTER PROCESSING keep-alives) are dropped anywhere", function()
    local stream = ": OPENROUTER PROCESSING\ndata: x\n: mid-event comment\ndata: y\n\n"
      .. ": trailing comment\n"
    local ref = assert_split_invariant(stream)
    assert.equal(1, #ref.events)
    assert.equal("x\ny", ref.events[1].data)
  end)

  it("event-named (Anthropic-shaped) stream is plain SSE", function()
    local stream = 'event: content_block_delta\ndata: {"delta":{"text":"hi"}}\n\n'
      .. 'event: ping\ndata: {"type":"ping"}\n\n'
      .. 'event: error\ndata: {"error":{"type":"overloaded_error","message":"busy"}}\n\n'
    local ref = assert_split_invariant(stream)
    assert.equal(3, #ref.events)
    assert.equal("content_block_delta", ref.events[1].type)
    assert.equal("ping", ref.events[2].type)
    assert.equal("error", ref.events[3].type)
  end)

  it("UTF-8 multi-byte content survives any split", function()
    local ref = assert_split_invariant("data: héllo \240\159\140\141 wörld\n\n")
    assert.equal("héllo \240\159\140\141 wörld", ref.events[1].data)
  end)

  it("BOM is consumed, including when split mid-BOM", function()
    local ref = assert_split_invariant("\239\187\191data: x\n\n")
    assert.equal(1, #ref.events)
    assert.equal("x", ref.events[1].data)
  end)

  it("a BOM false-start becomes line content", function()
    -- 0xEF not followed by 0xBB: the byte belongs to the (unknown) field name.
    local ref = assert_split_invariant("\239data: x\n\ndata: y\n\n")
    assert.equal(1, #ref.events)
    assert.equal("y", ref.events[1].data)
  end)
end)

describe("sse field processing", function()
  it("colon-less line is a field with empty value", function()
    local ref = run_stream({ "data: line1\ndata\n\n" })
    assert.equal("line1\n", ref.events[1].data)
    local lone = run_stream({ "data\n\n" })
    assert.equal(1, #lone.events)
    assert.equal("", lone.events[1].data)
  end)

  it("strips exactly one leading space from values", function()
    local ref = run_stream({ "data:x\n\ndata: x\n\ndata:  x\n\n" })
    assert.equal("x", ref.events[1].data)
    assert.equal("x", ref.events[2].data)
    assert.equal(" x", ref.events[3].data)
  end)

  it("event without data dispatches nothing", function()
    local ref = run_stream({ "event: lonely\n\ndata: real\n\n" })
    assert.equal(1, #ref.events)
    -- The lonely event's type was cleared by its blank line.
    assert.equal("message", ref.events[1].type)
    assert.equal("real", ref.events[1].data)
  end)

  it("ids persist across events; ids with NUL are ignored", function()
    local ref = run_stream({ "id: abc\ndata: one\n\ndata: two\n\nid: x\0y\ndata: three\n\n" })
    assert.equal("abc", ref.events[1].id)
    assert.equal("abc", ref.events[2].id)
    assert.equal("abc", ref.events[3].id) -- NUL id ignored, previous persists
  end)

  it("blank-only stream emits nothing and finishes clean", function()
    local ref = assert_split_invariant("\n\n\r\n\r")
    assert.equal(0, #ref.events)
    assert.is_true(ref.ok)
    assert.is_false(ref.truncated)
  end)
end)

describe("sse truncation", function()
  it("mid-line cut: prior events delivered, finish reports truncated", function()
    local ref = run_stream({ "data: one\n\ndata: par" })
    assert.equal(1, #ref.events)
    assert.is_true(ref.ok)
    assert.is_true(ref.truncated)
  end)

  it("complete data line but no blank line: truncated", function()
    local ref = run_stream({ "data: one\n\ndata: full\n" })
    assert.equal(1, #ref.events)
    assert.is_true(ref.truncated)
  end)
end)

describe("sse caps", function()
  local function limits(tbl)
    local l = ffi.new("SseLimits")
    for k, v in pairs(tbl) do
      l[k] = v
    end
    return l
  end

  it("line cap: overflow fails latched, independent of chunking", function()
    local long = "data: " .. ("x"):rep(70) .. "\n\n"
    local whole = run_stream({ long }, limits({ max_line = 64 }))
    assert.is_false(whole.ok)
    assert.truthy(whole.errmsg:match("line exceeds limit"))
    assert.is_true(whole.truncated)
    local bytewise = run_stream(explode(long), limits({ max_line = 64 }))
    assert.is_false(bytewise.ok)
    assert.truthy(bytewise.errmsg:match("line exceeds limit"))
  end)

  it("line cap: exactly at the cap succeeds", function()
    -- "data: " + 58 bytes = 64-byte line.
    local exact = "data: " .. ("x"):rep(58) .. "\n\n"
    local ref = run_stream({ exact }, limits({ max_line = 64 }))
    assert.is_true(ref.ok)
    assert.equal(1, #ref.events)
    assert.equal(58, #ref.events[1].data)
  end)

  it("event data cap counts the joining newline (value cap-1 fits, cap fails)", function()
    local ok15 =
      run_stream({ "data: " .. ("a"):rep(15) .. "\n\n" }, limits({ max_event_data = 16 }))
    assert.is_true(ok15.ok)
    assert.equal(1, #ok15.events)
    local over =
      run_stream({ "data: " .. ("a"):rep(16) .. "\n\n" }, limits({ max_event_data = 16 }))
    assert.is_false(over.ok)
    assert.truthy(over.errmsg:match("event data exceeds limit"))
  end)

  it("a failed parser refuses further input with the same error", function()
    local cb = ffi.cast("SseEventCb", function()
      return true
    end)
    local parser = lib.sse_parser_new(limits({ max_line = 8 }), cb, nil)
    local err = ffi.new("Error")
    err.type = ffi.C.kErrorTypeNone
    assert.is_false(lib.sse_parser_feed(parser, "0123456789", 10, err))
    lib.api_clear_error(err)
    assert.is_false(lib.sse_parser_feed(parser, "data: x\n\n", 9, err))
    assert.truthy(ffi.string(err.msg):match("line exceeds limit"))
    lib.api_clear_error(err)
    -- reset clears the latch and the parser works again
    lib.sse_parser_reset(parser)
    assert.is_true(lib.sse_parser_feed(parser, "ok\n", 3, err))
    lib.sse_parser_free(parser)
    cb:free()
  end)
end)

describe("sse abort and reuse", function()
  it("callback returning false aborts the stream", function()
    local ref = run_stream({ "data: one\n\ndata: two\n\ndata: three\n\n" }, nil, 2)
    assert.equal(2, #ref.events)
    assert.is_false(ref.ok)
    assert.truthy(ref.errmsg:match("aborted by event callback"))
    assert.is_true(ref.truncated)
  end)

  it("reset gives results identical to a fresh parser", function()
    local stream_a = join(OR_EVENTS, "\n")
    local stream_b = "data: after reset\n\n"
    local events = {}
    local cb = ffi.cast("SseEventCb", function(_, etype, data, id)
      events[#events + 1] = { type = view(etype), data = view(data), id = view(id) }
      return true
    end)
    local parser = lib.sse_parser_new(nil, cb, nil)
    local err = ffi.new("Error")
    err.type = ffi.C.kErrorTypeNone
    assert.is_true(lib.sse_parser_feed(parser, stream_a, #stream_a, err))
    local first_count = #events
    lib.sse_parser_reset(parser)
    assert.is_true(lib.sse_parser_feed(parser, stream_b, #stream_b, err))
    assert.equal(first_count + 1, #events)
    assert.equal("after reset", events[#events].data)
    assert.equal(ffi.C.kSseEofClean, lib.sse_parser_finish(parser))
    lib.sse_parser_free(parser)
    cb:free()
  end)
end)

describe("sse bounded stress", function()
  it("1000 events parse identically whole, prefix-bytewise, and random-split", function()
    local lines = {}
    for i = 1, 1000 do
      lines[#lines + 1] = ('data: {"choices":[{"delta":{"content":"chunk %d"}}]}'):format(i)
      lines[#lines + 1] = ""
    end
    local stream = join(lines, "\n")
    local ref = run_stream({ stream })
    assert.equal(1000, #ref.events)
    assert.is_false(ref.truncated)
    -- Byte-by-byte over the first 4 KiB, the rest as one chunk.
    local chunks = explode(stream:sub(1, 4096))
    chunks[#chunks + 1] = stream:sub(4097)
    assert.same(ref, run_stream(chunks))
    local prng = make_prng(0xC0FFEEDEADBEEFULL)
    for _ = 1, 20 do
      assert.same(ref, run_stream(random_splits(stream, prng)))
    end
  end)
end)
