-- A ToolPre hook that records it ran, then raises. The dispatch must catch it
-- (nonfatal): the throw is not a veto, so the tool still runs and the turn
-- completes. MUA_AUTOCMD_LOG names the marker file.
local logfile = os.getenv("MUA_AUTOCMD_LOG")

local function note(line)
  if not logfile or logfile == "" then
    return
  end
  local f = io.open(logfile, "a")
  if f then
    f:write(line .. "\n")
    f:close()
  end
end

mua.create_autocmd("ToolPre", {
  callback = function(ev)
    note("threw for " .. tostring(ev.tool))
    error("boom-in-toolpre")
  end,
})
