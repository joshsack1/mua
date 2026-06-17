-- A ToolPre hook that REWRITES a tool's arguments and returns the args table,
-- proving the rewritten args are what actually execute (not just observed).
-- read: swap a "_DECOY" path segment for "_REAL". bash: swap "original" for
-- "rewritten" in the command. The autocmd_spec asserts the executed args by
-- inspecting the tool result that reaches the second request.
mua.create_autocmd("ToolPre", {
  callback = function(ev)
    if ev.tool == "read" and type(ev.args.path) == "string" then
      ev.args.path = ev.args.path:gsub("_DECOY", "_REAL")
      return ev.args
    end
    if ev.tool == "bash" and type(ev.args.command) == "string" then
      ev.args.command = ev.args.command:gsub("original", "rewritten")
      return ev.args
    end
  end,
})
