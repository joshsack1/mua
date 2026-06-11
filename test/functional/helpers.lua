local M = {}

local function shell_quote(s)
  return "'" .. tostring(s):gsub("'", "'\\''") .. "'"
end

--- Run the mua binary and capture its output.
---
--- LuaJIT's 5.1-era `f:close()` does not reliably return the exit status, so
--- the status is captured in-band via a trailing marker on stdout and stderr
--- goes to a temp file.
---
---@param args table|nil argv list
---@param env table|nil extra environment variables
---@return table result { stdout = string, stderr = string, code = number }
function M.run_mua(args, env)
  local prg = assert(os.getenv("MUA_PRG"), "MUA_PRG not set (run via `make functionaltest`)")
  local stderr_file = os.tmpname()

  local cmd = {}
  if env and next(env) then
    cmd[#cmd + 1] = "env"
    for k, v in pairs(env) do
      cmd[#cmd + 1] = ("%s=%s"):format(k, shell_quote(v))
    end
  end
  cmd[#cmd + 1] = shell_quote(prg)
  for _, a in ipairs(args or {}) do
    cmd[#cmd + 1] = shell_quote(a)
  end
  local full = table.concat(cmd, " ") .. " 2>" .. shell_quote(stderr_file) .. '; echo "::exit::$?"'

  local pipe = assert(io.popen(full, "r"))
  local out = pipe:read("*a") or ""
  pipe:close()

  local code = tonumber(out:match("::exit::(%d+)%s*$"))
  out = out:gsub("::exit::%d+%s*$", "")

  local stderr_handle = io.open(stderr_file, "r")
  local err_out = ""
  if stderr_handle then
    err_out = stderr_handle:read("*a") or ""
    stderr_handle:close()
  end
  os.remove(stderr_file)

  return { stdout = out, stderr = err_out, code = code }
end

return M
