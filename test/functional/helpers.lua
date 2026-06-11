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

--- Serialize connection blocks into the sse_server script format.
local function serialize_blocks(blocks)
  local parts = {}
  for index, block in ipairs(blocks) do
    if index > 1 then
      parts[#parts + 1] = "next\n"
    end
    for _, dir in ipairs(block) do
      local kind = dir[1]
      if kind == "send" then
        parts[#parts + 1] = ("send %d\n"):format(#dir[2])
        parts[#parts + 1] = dir[2]
      elseif kind == "sleep" then
        parts[#parts + 1] = ("sleep %d\n"):format(dir[2])
      elseif kind == "close" or kind == "reset" then
        parts[#parts + 1] = kind .. "\n"
      else
        error("unknown directive kind: " .. tostring(kind))
      end
    end
  end
  return table.concat(parts)
end

local function parse_captured_request(raw)
  local head_end = raw:find("\r\n\r\n", 1, true)
  local head = head_end and raw:sub(1, head_end - 1) or raw
  local body = head_end and raw:sub(head_end + 4) or ""
  local method, path = head:match("^(%u+) (%S+) HTTP/1%.1")
  local headers = {}
  for line in head:gmatch("\r\n([^\r\n]+)") do
    local name, value = line:match("^([^:]+):%s*(.*)$")
    if name then
      headers[name:lower()] = value
    end
  end
  return { raw = raw, method = method, path = path, headers = headers, body = body }
end

--- Start the SSE fixture server with a scripted wire personality.
---
--- Every scripted response MUST include "Connection: close" and rely on
--- close-delimited framing: chunked encoding would let curl re-frame the
--- scripted byte slices, and a keep-alive connection could be reused across
--- retry attempts out of curl's connection cache, breaking the
--- one-block-per-attempt model.
---
---@param blocks table connection blocks; directives are
---  {"send", bytes} | {"sleep", ms} | {"close"} | {"reset"}
---@param opts table|nil { timeout_ms = number } (default 10000)
---@return table srv { port, url, finish() -> { code, requests } }
function M.start_sse_server(blocks, opts)
  local server_bin = os.getenv("MUA_SSE_SERVER") or "build/bin/mua_sse_server"
  local script_file = os.tmpname()
  local capture_file = os.tmpname()
  local script = assert(io.open(script_file, "wb"))
  assert(script:write(serialize_blocks(blocks)))
  assert(script:close())

  local timeout_ms = (opts and opts.timeout_ms) or 10000
  local cmd = ("%s --script %s --capture %s --timeout-ms %d"):format(
    shell_quote(server_bin),
    shell_quote(script_file),
    shell_quote(capture_file),
    timeout_ms
  ) .. '; echo "::exit::$?"'
  local pipe = assert(io.popen(cmd, "r"))
  local first_line = pipe:read("*l")
  local port = first_line and first_line:match("^PORT (%d+)$")
  if not port then
    pipe:close()
    os.remove(script_file)
    os.remove(capture_file)
    error("sse server failed to start: " .. tostring(first_line))
  end

  local srv = { port = tonumber(port), url = "http://127.0.0.1:" .. port }

  --- Wait for the server to exit; returns its exit code and the captured
  --- requests, each parsed as { raw, method, path, headers (lowercased), body }.
  function srv.finish()
    local rest = pipe:read("*a") or ""
    pipe:close()
    local code = tonumber(rest:match("::exit::(%d+)%s*$"))
    local requests = {}
    local capture = io.open(capture_file, "rb")
    if capture then
      local data = capture:read("*a") or ""
      capture:close()
      local pos = 1
      while true do
        local _, header_end, len = data:find("^REQUEST %d+ (%d+)\n", pos)
        if not header_end then
          break
        end
        len = tonumber(len)
        requests[#requests + 1] = parse_captured_request(data:sub(header_end + 1, header_end + len))
        pos = header_end + len + 2 -- skip the raw bytes and the trailing newline
      end
    end
    os.remove(script_file)
    os.remove(capture_file)
    return { code = code, requests = requests }
  end

  return srv
end

return M
