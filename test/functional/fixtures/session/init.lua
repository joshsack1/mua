-- Session-scope functional fixture. Each hook calls mua.sess.get_messages(0) /
-- mua.sess.get_id(0) and appends what it observed to MUA_SESS_LOG, so the spec
-- can assert the conversation the API returned at SessionStart (empty), at
-- ToolPre (mid-turn), and at SessionEnd (the full turn).
local logfile = os.getenv("MUA_SESS_LOG")

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

local function record(prefix)
  local msgs = mua.sess.get_messages(0)
  note(prefix .. "_count=" .. #msgs)
  note(prefix .. "_id=" .. tostring(mua.sess.get_id(0)))
  for i, m in ipairs(msgs) do
    note(prefix .. "_role" .. i .. "=" .. tostring(m.role))
  end
  if msgs[1] then
    note(prefix .. "_first_content=" .. tostring(msgs[1].content))
  end
end

mua.create_autocmd("SessionStart", {
  callback = function(ev)
    note("ev_session=" .. tostring(ev.session)) -- the autocmd payload's id
    record("start")
  end,
})

mua.create_autocmd("ToolPre", {
  callback = function()
    record("pre")
  end,
})

mua.create_autocmd("SessionEnd", {
  callback = function()
    record("end")
  end,
})
