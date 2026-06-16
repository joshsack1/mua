-- Registers the tools the functional tools_spec drives the model to call. Each
-- exercises one path: a string result, a JSON-encoded table result, a raising
-- callback, and the approval gate (mutating true vs false).

mua.register_tool({
  name = "echo_tool",
  description = "Echo the text argument back",
  schema = {
    type = "object",
    properties = { text = { type = "string" } },
    required = { "text" },
  },
  mutating = false, -- ungated: runs under -p without --yes
  callback = function(args)
    return "echoed:" .. tostring(args.text)
  end,
})

mua.register_tool({
  name = "table_tool",
  description = "Return a structured result (JSON-encoded into the tool content)",
  -- no schema => defaults to an empty-object schema
  mutating = false,
  callback = function()
    return { greeting = "hi-from-table", n = 3 }
  end,
})

mua.register_tool({
  name = "boom_tool",
  description = "Always raises",
  mutating = false,
  callback = function()
    error("boom-from-callback")
  end,
})

mua.register_tool({
  name = "gated_tool",
  description = "A mutating tool, so it goes through the approval gate",
  mutating = true,
  callback = function()
    return "gated-ran"
  end,
})
