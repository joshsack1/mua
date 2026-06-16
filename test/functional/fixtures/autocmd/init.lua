-- Autocmd hooks for the functional autocmd_spec. Each appends a marker line to
-- the file named by MUA_AUTOCMD_LOG so the test can observe that it fired. The
-- ToolPre hook also vetoes a read whose path mentions "secret".
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

mua.create_autocmd("SessionStart", {
  callback = function(ev)
    note("start " .. tostring(ev.session))
  end,
})

mua.create_autocmd("SessionEnd", {
  callback = function(ev)
    note("end " .. tostring(ev.session))
  end,
})

mua.create_autocmd("StreamDelta", {
  callback = function(ev)
    note("delta " .. tostring(ev.text))
  end,
})

mua.create_autocmd("ToolPre", {
  callback = function(ev)
    note("pre " .. tostring(ev.tool))
    local path = ev.args and ev.args.path
    if ev.tool == "read" and type(path) == "string" and path:find("secret", 1, true) then
      return "blocked: secret file"
    end
  end,
})

mua.create_autocmd("ToolPost", {
  callback = function(ev)
    note("post " .. tostring(ev.tool) .. " err=" .. tostring(ev.error))
  end,
})
