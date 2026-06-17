local t = require("test.unit.helpers")
local ffi = t.ffi
local lib = t.lib

-- Declarations mirror src/mua/provider/openrouter{,_internal}.h (cJSON and
-- json_print/json_free come from helpers).
t.cdef([[
  typedef struct OpenrouterStream OpenrouterStream;
  typedef struct {
    int64_t prompt_tokens;
    int64_t completion_tokens;
    int64_t total_tokens;
  } Usage;
  typedef struct {
    void (*on_text)(void *ud, const String *text);
    void (*on_done)(void *ud, cJSON *message, const String *finish_reason,
                    const Usage *usage);
    void (*on_error)(void *ud, const Error *err);
  } OpenrouterCallbacks;
  bool openrouter_handle_event(OpenrouterStream *stream, String data);
  OpenrouterStream *openrouter_stream_new_for_test(const OpenrouterCallbacks *cb, void *ud);
  void openrouter_stream_free_for_test(OpenrouterStream *stream);
  bool openrouter_stream_delivered_for_test(const OpenrouterStream *stream);
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
--- and feeds events through the real dispatch path. on_done prints and frees
--- the handed-over message (ownership transfers to the callee).
local function make_harness()
  local h = { texts = {}, done = nil, errors = {} }
  h.cbs = ffi.new("OpenrouterCallbacks")
  h.on_text = ffi.cast("void (*)(void *, const String *)", function(_, text)
    h.texts[#h.texts + 1] = view(text)
  end)
  h.on_done = ffi.cast(
    "void (*)(void *, cJSON *, const String *, const Usage *)",
    function(_, message, reason, usage)
      assert.is_nil(h.done, "on_done fired twice")
      local printed = lib.json_print(message)
      local text = ffi.string(printed.data, printed.size)
      lib.xfree(printed.data)
      lib.json_free(message)
      h.done = {
        message = text,
        reason = view(reason),
        tokens = tonumber(usage.completion_tokens),
        prompt = tonumber(usage.prompt_tokens),
        total = tonumber(usage.total_tokens),
      }
    end
  )
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
  h.delivered = function(self)
    return lib.openrouter_stream_delivered_for_test(self.stream)
  end
  h.free = function(self)
    lib.openrouter_stream_free_for_test(self.stream)
    self.on_text:free()
    self.on_done:free()
    self.on_error:free()
  end
  return h
end

--- Feed one tool_call delta event built from an items-JSON string.
local function tool_call_event(items_json)
  return '{"choices":[{"delta":{"tool_calls":[' .. items_json .. "]}}]}"
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
    assert.equal("stop", h.done.reason)
    assert.equal(0, h.done.tokens) -- no usage object in this stream
    assert.equal(0, h.done.prompt)
    assert.equal(0, h.done.total)
    -- Content-only response: exact wire shape, no tool_calls key at all.
    assert.equal('{"role":"assistant","content":"Hello world"}', h.done.message)
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

  it("a usage chunk (empty choices) latches prompt/completion/total tokens", function()
    local h = make_harness()
    assert.is_true(h:feed('{"choices":[{"delta":{"content":"x"},"finish_reason":"stop"}]}'))
    assert.is_true(
      h:feed(
        '{"choices":[],"usage":{"prompt_tokens":3,"completion_tokens":42,"total_tokens":45}}'
      )
    )
    assert.is_false(h:feed("[DONE]"))
    assert.equal("stop", h.done.reason)
    assert.equal(42, h.done.tokens)
    assert.equal(3, h.done.prompt)
    assert.equal(45, h.done.total)
    assert.equal('{"role":"assistant","content":"x"}', h.done.message)
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

describe("openrouter tool_call accumulation", function()
  after_each(function()
    anchors = {}
  end)

  it("concatenates split arguments into the exact assembled message", function()
    local h = make_harness()
    assert.is_true(
      h:feed(
        tool_call_event(
          '{"index":0,"id":"call_abc","type":"function","function":{"name":"bash","arguments":""}}'
        )
      )
    )
    assert.is_true(h:feed(tool_call_event('{"index":0,"function":{"arguments":"{\\"com"}}')))
    assert.is_true(
      h:feed(tool_call_event('{"index":0,"function":{"arguments":"mand\\":\\"ls\\"}"}}'))
    )
    assert.is_true(h:feed('{"choices":[{"delta":{},"finish_reason":"tool_calls"}]}'))
    assert.is_false(h:feed("[DONE]"))
    assert.equal("tool_calls", h.done.reason)
    assert.equal(
      '{"role":"assistant","content":null,"tool_calls":[{"id":"call_abc","type":"function",'
        .. '"function":{"name":"bash","arguments":"{\\"command\\":\\"ls\\"}"}}]}',
      h.done.message
    )
    assert.same({}, h.errors)
    h:free()
  end)

  it("assembles interleaved parallel calls in index order", function()
    local h = make_harness()
    assert.is_true(
      h:feed(
        tool_call_event(
          '{"index":0,"id":"call_a","type":"function","function":{"name":"bash","arguments":""}}'
        )
      )
    )
    assert.is_true(
      h:feed(
        tool_call_event(
          '{"index":1,"id":"call_b","type":"function","function":{"name":"read","arguments":""}}'
        )
      )
    )
    assert.is_true(h:feed(tool_call_event('{"index":1,"function":{"arguments":"{\\"b\\":2}"}}')))
    assert.is_true(h:feed(tool_call_event('{"index":0,"function":{"arguments":"{\\"a\\":1}"}}')))
    assert.is_false(h:feed("[DONE]"))
    assert.equal(
      '{"role":"assistant","content":null,"tool_calls":['
        .. '{"id":"call_a","type":"function","function":{"name":"bash","arguments":"{\\"a\\":1}"}},'
        .. '{"id":"call_b","type":"function","function":{"name":"read","arguments":"{\\"b\\":2}"}}]}',
      h.done.message
    )
    h:free()
  end)

  it("carries content and tool_calls together", function()
    local h = make_harness()
    assert.is_true(h:feed('{"choices":[{"delta":{"content":"Sure."}}]}'))
    assert.is_true(
      h:feed(
        tool_call_event(
          '{"index":0,"id":"call_1","type":"function","function":{"name":"bash","arguments":"{}"}}'
        )
      )
    )
    assert.is_false(h:feed("[DONE]"))
    assert.equal(
      '{"role":"assistant","content":"Sure.","tool_calls":'
        .. '[{"id":"call_1","type":"function","function":{"name":"bash","arguments":"{}"}}]}',
      h.done.message
    )
    assert.same({ "Sure." }, h.texts) -- deltas still streamed live
    h:free()
  end)

  it("skips an index gap and keeps index order", function()
    local h = make_harness()
    assert.is_true(
      h:feed(
        tool_call_event(
          '{"index":0,"id":"call_0","type":"function","function":{"name":"bash","arguments":"{}"}}'
        )
      )
    )
    assert.is_true(
      h:feed(
        tool_call_event(
          '{"index":2,"id":"call_2","type":"function","function":{"name":"read","arguments":"{}"}}'
        )
      )
    )
    assert.is_false(h:feed("[DONE]"))
    assert.equal(
      '{"role":"assistant","content":null,"tool_calls":['
        .. '{"id":"call_0","type":"function","function":{"name":"bash","arguments":"{}"}},'
        .. '{"id":"call_2","type":"function","function":{"name":"read","arguments":"{}"}}]}',
      h.done.message
    )
    h:free()
  end)

  it("missing, negative, or out-of-range index is terminal", function()
    for _, item in ipairs({
      '{"function":{"arguments":"x"}}',
      '{"index":-1,"function":{"arguments":"x"}}',
      '{"index":16,"function":{"arguments":"x"}}',
    }) do
      local h = make_harness()
      assert.is_false(h:feed(tool_call_event(item)))
      assert.equal(1, #h.errors)
      assert.truthy(h.errors[1]:match("index"))
      assert.is_nil(h.done)
      h:free()
    end
  end)

  it("more than 64 tool_call items in one event is terminal", function()
    local items = {}
    for _ = 1, 65 do
      items[#items + 1] = '{"index":0,"function":{"arguments":"x"}}'
    end
    local h = make_harness()
    assert.is_false(h:feed(tool_call_event(table.concat(items, ","))))
    assert.equal(1, #h.errors)
    assert.truthy(h.errors[1]:match("too many"))
    h:free()
  end)

  it("oversize id, name, arguments, and content are terminal, never truncated", function()
    local cases = {
      { item = '{"index":0,"id":"' .. string.rep("i", 64) .. '"}', pattern = "id" },
      {
        item = '{"index":0,"function":{"name":"' .. string.rep("n", 128) .. '"}}',
        pattern = "name",
      },
      {
        item = '{"index":0,"function":{"arguments":"' .. string.rep("a", 262145) .. '"}}',
        pattern = "arguments",
      },
    }
    for _, case in ipairs(cases) do
      local h = make_harness()
      assert.is_false(h:feed(tool_call_event(case.item)))
      assert.equal(1, #h.errors)
      assert.truthy(h.errors[1]:match(case.pattern))
      h:free()
    end
    -- content: three 400 KiB deltas blow the 1 MiB accumulator on the third
    local h = make_harness()
    local big = '{"choices":[{"delta":{"content":"' .. string.rep("c", 400000) .. '"}}]}'
    assert.is_true(h:feed(big))
    assert.is_true(h:feed(big))
    assert.is_false(h:feed(big))
    assert.equal(1, #h.errors)
    assert.truthy(h.errors[1]:match("content"))
    h:free()
  end)

  it("a present call with no name is incomplete: on_error, not on_done", function()
    local h = make_harness()
    assert.is_true(h:feed(tool_call_event('{"index":0,"id":"call_x"}')))
    assert.is_false(h:feed("[DONE]"))
    assert.is_nil(h.done)
    assert.equal(1, #h.errors)
    assert.truthy(h.errors[1]:match("incomplete"))
    h:free()
  end)

  it("tool_call fragments set delivered, like content does", function()
    local h = make_harness()
    assert.is_false(h:delivered())
    assert.is_true(h:feed(tool_call_event('{"index":0,"id":"call_d"}')))
    assert.is_true(h:delivered())
    h:free()
    local h2 = make_harness()
    assert.is_false(h2:delivered())
    assert.is_true(h2:feed('{"choices":[{"delta":{"content":"x"}}]}'))
    assert.is_true(h2:delivered())
    h2:free()
  end)

  -- Captured verbatim from a live OpenRouter stream (anthropic/claude-sonnet-4.6
  -- served via Google, 2026-06-12): the wire-shape hedge for the accumulator.
  -- Notable quirks pinned here: delta.content is null alongside tool_calls, a
  -- continuation may carry an empty arguments fragment, and the usage chunk
  -- repeats the finish_reason with non-empty choices.
  it("assembles a captured live stream byte-for-byte", function()
    local h = make_harness()
    local events = {
      [=[{"id":"gen-1781299023-2l9AJkoIKYe4amGiNYDp","object":"chat.completion.chunk","created":1781299023,"model":"anthropic/claude-4.6-sonnet-20260217","provider":"Google","choices":[{"index":0,"delta":{"content":null,"role":"assistant","tool_calls":[{"index":0,"id":"toolu_vrtx_01TGPD8BXsALGUUK1j1qoDKt","type":"function","function":{"name":"bash","arguments":""}}]},"finish_reason":null,"native_finish_reason":null}]}]=],
      [=[{"id":"gen-1781299023-2l9AJkoIKYe4amGiNYDp","object":"chat.completion.chunk","created":1781299023,"model":"anthropic/claude-4.6-sonnet-20260217","provider":"Google","choices":[{"index":0,"delta":{"content":null,"role":"assistant","tool_calls":[{"index":0,"function":{"arguments":""}}]},"finish_reason":null,"native_finish_reason":null}]}]=],
      [=[{"id":"gen-1781299023-2l9AJkoIKYe4amGiNYDp","object":"chat.completion.chunk","created":1781299023,"model":"anthropic/claude-4.6-sonnet-20260217","provider":"Google","choices":[{"index":0,"delta":{"content":null,"role":"assistant","tool_calls":[{"index":0,"function":{"arguments":"{\"command\": \"ls"}}]},"finish_reason":null,"native_finish_reason":null}]}]=],
      [=[{"id":"gen-1781299023-2l9AJkoIKYe4amGiNYDp","object":"chat.completion.chunk","created":1781299023,"model":"anthropic/claude-4.6-sonnet-20260217","provider":"Google","choices":[{"index":0,"delta":{"content":null,"role":"assistant","tool_calls":[{"index":0,"function":{"arguments":"\"}"}}]},"finish_reason":null,"native_finish_reason":null}]}]=],
      [=[{"id":"gen-1781299023-2l9AJkoIKYe4amGiNYDp","object":"chat.completion.chunk","created":1781299023,"model":"anthropic/claude-4.6-sonnet-20260217","provider":"Google","choices":[{"index":0,"delta":{"content":"","role":"assistant"},"finish_reason":"tool_calls","native_finish_reason":"tool_use"}]}]=],
      [=[{"id":"gen-1781299023-2l9AJkoIKYe4amGiNYDp","object":"chat.completion.chunk","created":1781299023,"model":"anthropic/claude-4.6-sonnet-20260217","provider":"Google","service_tier":null,"choices":[{"index":0,"delta":{"content":"","role":"assistant"},"finish_reason":"tool_calls","native_finish_reason":"tool_use"}],"usage":{"prompt_tokens":578,"completion_tokens":52,"total_tokens":630,"cost":0.002514,"is_byok":false,"prompt_tokens_details":{"cached_tokens":0,"cache_write_tokens":0,"audio_tokens":0,"video_tokens":0},"cost_details":{"upstream_inference_cost":0.002514,"upstream_inference_prompt_cost":0.001734,"upstream_inference_completions_cost":0.00078},"completion_tokens_details":{"reasoning_tokens":0,"image_tokens":0,"audio_tokens":0}}}]=],
    }
    for _, event in ipairs(events) do
      assert.is_true(h:feed(event))
    end
    assert.is_false(h:feed("[DONE]"))
    assert.equal("tool_calls", h.done.reason)
    assert.equal(52, h.done.tokens)
    assert.equal(578, h.done.prompt)
    assert.equal(630, h.done.total)
    assert.equal(
      [=[{"role":"assistant","content":null,"tool_calls":[{"id":"toolu_vrtx_01TGPD8BXsALGUUK1j1qoDKt","type":"function","function":{"name":"bash","arguments":"{\"command\": \"ls\"}"}}]}]=],
      h.done.message
    )
    assert.same({}, h.texts) -- content stayed null/empty throughout
    assert.same({}, h.errors)
    h:free()
  end)
end)
