-- A ToolPre hook that APPROVES a bash call outright (returns true), so the tool
-- runs with no approval prompt and without --yes -- proving the approve outcome
-- skips the base gate. Any other tool falls through (nil) to the normal gate.
-- The autocmd_spec asserts no "allow bash" prompt fired and the command ran.
mua.create_autocmd("ToolPre", {
  callback = function(ev)
    if ev.tool == "bash" then
      return true -- approve: skip the base gate, no y/N prompt
    end
  end,
})
