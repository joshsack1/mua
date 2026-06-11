local t = require("test.unit.helpers")
local ffi = t.ffi
local lib = t.lib

-- Declarations mirror src/mua/provider/openrouter{,_internal}.h.
t.cdef([[
  typedef struct OpenrouterStream OpenrouterStream;
  typedef struct {
    void (*on_text)(void *ud, const String *text);
    void (*on_done)(void *ud, const String *finish_reason, int64_t completion_tokens);
    void (*on_error)(void *ud, const Error *err);
  } OpenrouterCallbacks;
  bool openrouter_handle_event(OpenrouterStream *stream, String data);
  OpenrouterStream *openrouter_stream_new_for_test(const OpenrouterCallbacks *cb, void *ud);
  void openrouter_stream_free_for_test(OpenrouterStream *stream);
]])

local anchors = {}

local function S(str)
  anchors[#anchors + 1] = str
  return ffi.new("String", ffi.cast("char *", str), #str)
end

local function view(s)
  return s.size == 0 and "" or ffi.string(s.data, s.size)
end

--- A harness around a bare (no-network) stream: records callback activity
--- and feeds events through the real dispatch path.
local function make_harness()
  local h = { texts = {}, done = nil, errors = {} }
  h.cbs = ffi.new("OpenrouterCallbacks")
  h.on_text = ffi.cast("void (*)(void *, const String *)", function(_, text)
    h.texts[#h.texts + 1] = view(text)
  end)
  h.on_done = ffi.cast("void (*)(void *, const String *, int64_t)", function(_, reason, tokens)
    assert.is_nil(h.done, "on_done fired twice")
    h.done = { reason = view(reason), tokens = tonumber(tokens) }
  end)
  h.on_error = ffi.cast("void (*)(void *, const Error *)", function(_, err)
    h.errors[#h.errors + 1] = err.msg ~= nil and ffi.string(err.msg) or "(no message)"
  end)
  h.cbs.on_text = h.on_text
  h.cbs.on_done = h.on_done
  h.cbs.on_error = h.on_error
  h.stream = lib.openrouter_stream_new_for_test(h.cbs, nil)
  h.feed = function(self, data)
    return lib.openrouter_handle_event(self.stream, S(data))
  end
  h.free = function(self)
    lib.openrouter_stream_free_for_test(self.stream)
    self.on_text:free()
    self.on_done:free()
    self.on_error:free()
  end
  return h
end

describe("openrouter event dispatch", function()
  after_each(function()
    anchors = {}
  end)

  it("streams delta text in order and finishes on [DONE]", function()
    local h = make_harness()
    assert.is_true(h:feed('{"choices":[{"delta":{"content":"Hel"},"finish_reason":null}]}'))
    assert.is_true(h:feed('{"choices":[{"delta":{"content":"lo "}}]}'))
    assert.is_true(h:feed('{"choices":[{"delta":{"content":"world"},"finish_reason":"stop"}]}'))
    assert.is_false(h:feed("[DONE]")) -- sentinel stops the transfer
    assert.same({ "Hel", "lo ", "world" }, h.texts)
    assert.same({ reason = "stop", tokens = 0 }, h.done)
    assert.same({}, h.errors)
    -- Anything after the terminal event is refused without callbacks.
    assert.is_false(h:feed('{"choices":[{"delta":{"content":"late"}}]}'))
    assert.same({ "Hel", "lo ", "world" }, h.texts)
    h:free()
  end)

  it("only the exact sentinel terminates; lookalikes are malformed JSON", function()
    for _, lookalike in ipairs({ "[DONE] ", "[done]" }) do
      local h = make_harness()
      assert.is_false(h:feed(lookalike))
      assert.is_nil(h.done)
      assert.equal(1, #h.errors)
      h:free()
    end
  end)

  it("a usage chunk (empty choices) latches completion_tokens", function()
    local h = make_harness()
    assert.is_true(h:feed('{"choices":[{"delta":{"content":"x"},"finish_reason":"stop"}]}'))
    assert.is_true(h:feed('{"choices":[],"usage":{"prompt_tokens":3,"completion_tokens":42}}'))
    assert.is_false(h:feed("[DONE]"))
    assert.same({ reason = "stop", tokens = 42 }, h.done)
    h:free()
  end)

  it("an error-in-data chunk is terminal and blocks any later on_done", function()
    local h = make_harness()
    assert.is_true(h:feed('{"choices":[{"delta":{"content":"partial"}}]}'))
    assert.is_false(h:feed('{"error":{"message":"Insufficient credits","code":402}}'))
    assert.equal(1, #h.errors)
    assert.truthy(h.errors[1]:match("Insufficient credits"))
    assert.is_false(h:feed("[DONE]"))
    assert.is_nil(h.done)
    h:free()
  end)

  it("empty or missing delta.content emits no on_text", function()
    local h = make_harness()
    assert.is_true(h:feed('{"choices":[{"delta":{"content":""}}]}'))
    assert.is_true(h:feed('{"choices":[{"delta":{}}]}'))
    assert.is_true(h:feed('{"choices":[{"delta":{"role":"assistant"}}]}'))
    assert.same({}, h.texts)
    assert.same({}, h.errors)
    h:free()
  end)

  it("malformed event JSON is a terminal error", function()
    local h = make_harness()
    assert.is_false(h:feed('{"choices":['))
    assert.equal(1, #h.errors)
    assert.truthy(h.errors[1]:match("json"))
    h:free()
  end)

  it("unknown extra fields are accepted liberally", function()
    local h = make_harness()
    assert.is_true(h:feed('{"id":"gen-1","provider":"x","unknown":[1,2],"choices":[]}'))
    assert.same({}, h.texts)
    assert.same({}, h.errors)
    h:free()
  end)
end)
