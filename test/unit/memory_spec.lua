local t = require("test.unit.helpers")
local ffi = t.ffi
local lib = t.lib

-- Declarations mirror src/mua/memory.h.
t.cdef([[
  void *xmalloc(size_t size);
  void *xcalloc(size_t count, size_t size);
  void *xrealloc(void *ptr, size_t size);
  char *xstrdup(const char *str);
  char *xstrndup(const char *str, size_t n);
  void *xmemdup(const void *data, size_t len);
  void xfree(void *ptr);

  typedef struct arena_block ArenaBlock;
  typedef struct { ArenaBlock *current; size_t offset; } Arena;
  void *arena_alloc(Arena *arena, size_t size);
  void arena_finish(Arena *arena);

  typedef struct { char *data; size_t size; size_t cap; size_t max; } Buf;
  void buf_init(Buf *buf, size_t max);
  bool buf_append(Buf *buf, const char *bytes, size_t len);
  void buf_reset(Buf *buf);
  void buf_free(Buf *buf);
]])

describe("xmalloc family", function()
  it("xmalloc(0) returns a usable non-NULL pointer", function()
    local p = lib.xmalloc(0)
    assert.is_true(p ~= nil)
    lib.xfree(p)
  end)

  it("xstrdup copies content independently", function()
    local src = "hello mua"
    local copy = lib.xstrdup(src)
    assert.equal(src, ffi.string(copy))
    lib.xfree(copy)
  end)

  it("xstrndup truncates at n bytes", function()
    local s = lib.xstrndup("hello", 3)
    assert.equal("hel", ffi.string(s))
    lib.xfree(s)
    local untruncated = lib.xstrndup("hi", 10)
    assert.equal("hi", ffi.string(untruncated))
    lib.xfree(untruncated)
  end)

  it("xmemdup copies exact byte ranges including NULs", function()
    local bytes = "a\0b"
    local copy = lib.xmemdup(bytes, 3)
    assert.equal("a\0b", ffi.string(copy, 3))
    lib.xfree(copy)
  end)

  it("xfree(NULL) is safe", function()
    lib.xfree(nil)
  end)
end)

describe("Arena", function()
  it("returns 16-byte-aligned pointers across block sizes", function()
    local arena = ffi.new("Arena") -- zero-init == ARENA_INIT
    local sizes = { 1, 10, 100, 4095, 8192 }
    for _, size in ipairs(sizes) do
      local p = lib.arena_alloc(arena, size)
      assert.is_true(p ~= nil)
      assert.equal(0, tonumber(ffi.cast("uintptr_t", p) % 16))
    end
    lib.arena_finish(arena)
  end)

  it("is reusable after arena_finish", function()
    local arena = ffi.new("Arena")
    assert.is_true(lib.arena_alloc(arena, 32) ~= nil)
    lib.arena_finish(arena)
    assert.equal(nil, arena.current ~= nil and arena.current or nil)
    assert.is_true(lib.arena_alloc(arena, 32) ~= nil)
    lib.arena_finish(arena)
  end)
end)

describe("Buf", function()
  it("appends up to exactly the cap and refuses beyond", function()
    local buf = ffi.new("Buf")
    lib.buf_init(buf, 8)
    assert.is_true(lib.buf_append(buf, "abcd", 4))
    assert.is_true(lib.buf_append(buf, "efgh", 4)) -- exactly at the cap
    assert.is_false(lib.buf_append(buf, "i", 1)) -- one past the cap
    assert.equal(8, tonumber(buf.size))
    assert.equal("abcdefgh", ffi.string(buf.data, buf.size))
    lib.buf_free(buf)
  end)

  it("keeps capacity across reset and stays usable", function()
    local buf = ffi.new("Buf")
    lib.buf_init(buf, 1024)
    assert.is_true(lib.buf_append(buf, ("x"):rep(100), 100))
    local cap_before = tonumber(buf.cap)
    lib.buf_reset(buf)
    assert.equal(0, tonumber(buf.size))
    assert.equal(cap_before, tonumber(buf.cap))
    assert.is_true(lib.buf_append(buf, "fresh", 5))
    assert.equal("fresh", ffi.string(buf.data, buf.size))
    lib.buf_free(buf)
  end)

  it("zero-length append always succeeds", function()
    local buf = ffi.new("Buf")
    lib.buf_init(buf, 0)
    assert.is_true(lib.buf_append(buf, "", 0))
    assert.is_false(lib.buf_append(buf, "a", 1))
    lib.buf_free(buf)
  end)
end)
