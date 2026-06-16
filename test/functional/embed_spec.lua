local helpers = require("test.functional.helpers")

-- End-to-end coverage of `mua --embed`: a tiny in-spec msgpack codec builds
-- request frames, the binary is driven with them on stdin (through a real pipe,
-- which is what uv_pipe expects), and the response frames are decoded and
-- asserted. The codec covers only the subset the protocol uses here (fixint,
-- uint8, nil, fixstr/str8, fixarray), doubling as wire-format documentation.

local function shq(s)
  return "'" .. tostring(s):gsub("'", "'\\''") .. "'"
end

-- --- minimal msgpack encode (requests) ---
local function mp_uint(n) -- small non-negative ints: fixint, else uint8
  if n <= 0x7f then
    return string.char(n)
  end
  return string.char(0xcc, n)
end

local function mp_str(s)
  if #s <= 31 then
    return string.char(0xa0 + #s) .. s
  end
  return string.char(0xd9, #s) .. s -- str8 (len <= 255 suffices here)
end

local function mp_array(encoded) -- encoded: list of already-encoded byte strings
  assert(#encoded <= 15)
  return string.char(0x90 + #encoded) .. table.concat(encoded)
end

-- A request frame [0, msgid, method, [string params...]].
local function request(msgid, method, params)
  local penc = {}
  for _, p in ipairs(params) do
    penc[#penc + 1] = mp_str(p)
  end
  return mp_array({ mp_uint(0), mp_uint(msgid), mp_str(method), mp_array(penc) })
end

-- --- minimal msgpack decode (responses) ---
-- Returns value, next_pos. A decoded msgpack nil yields a Lua nil (arrays simply
-- leave that index unset). Handles the subset responses use.
local function decode(s, pos)
  local b = s:byte(pos)
  if b <= 0x7f then
    return b, pos + 1
  elseif b == 0xcc then
    return s:byte(pos + 1), pos + 2
  elseif b == 0xc0 then
    return nil, pos + 1
  elseif b >= 0xa0 and b <= 0xbf then
    local n = b - 0xa0
    return s:sub(pos + 1, pos + n), pos + 1 + n
  elseif b == 0xd9 then -- str8
    local n = s:byte(pos + 1)
    return s:sub(pos + 2, pos + 1 + n), pos + 2 + n
  elseif b == 0xda then -- str16 (the encoder's next size up from fixstr)
    local n = s:byte(pos + 1) * 256 + s:byte(pos + 2)
    return s:sub(pos + 3, pos + 2 + n), pos + 3 + n
  elseif b == 0xdb then -- str32
    local n = ((s:byte(pos + 1) * 256 + s:byte(pos + 2)) * 256 + s:byte(pos + 3)) * 256
      + s:byte(pos + 4)
    return s:sub(pos + 5, pos + 4 + n), pos + 5 + n
  elseif b >= 0x90 and b <= 0x9f then
    local arr, p = {}, pos + 1
    for i = 1, b - 0x90 do
      arr[i], p = decode(s, p)
    end
    return arr, p
  end
  error(("decode: unexpected byte 0x%02x at offset %d"):format(b, pos))
end

-- Runs `mua --embed` with `stdin_bytes` piped in (a real pipe), returns the raw
-- stdout bytes and the exit code.
local function run_embed(stdin_bytes, config_dir)
  local prg = assert(os.getenv("MUA_PRG"), "MUA_PRG not set (run via `make functionaltest`)")
  local infile = os.tmpname()
  local f = assert(io.open(infile, "wb"))
  assert(f:write(stdin_bytes))
  assert(f:close())
  local stderr_file = os.tmpname()
  local state = helpers.tmpdir()
  local env = {
    MUA_CONFIG_DIR = config_dir,
    MUA_STATE_DIR = state,
    MUA_SYSTEM_PROMPT = "",
    MUA_LOG = "",
    OPENROUTER_API_KEY = "", -- --embed needs no key
  }
  local parts = { "cat", shq(infile), "|", "env" }
  for k, v in pairs(env) do
    parts[#parts + 1] = ("%s=%s"):format(k, shq(v))
  end
  parts[#parts + 1] = shq(prg)
  parts[#parts + 1] = "--embed"
  local cmd = table.concat(parts, " ") .. " 2>" .. shq(stderr_file) .. '; echo "::exit::$?"'
  local pipe = assert(io.popen(cmd, "r"))
  local out = pipe:read("*a") or ""
  pipe:close()
  local code = tonumber(out:match("::exit::(%d+)%s*$"))
  out = out:gsub("::exit::%d+%s*$", "")
  os.remove(infile)
  os.remove(stderr_file)
  helpers.rm_rf(state)
  return out, code
end

local FIX = "test/functional/fixtures/embed"

describe("--embed msgpack-RPC server", function()
  it("answers requests, and the same table drives RPC and the Lua var store", function()
    local stdin_bytes = request(0, "mua_set_option", { "model", "test-model-x" })
      .. request(1, "mua_get_option", { "model" })
      .. request(2, "mua_get_var", { "greeting" })
    local out, code = run_embed(stdin_bytes, FIX)
    assert.equal(0, code)

    local r1, p = decode(out, 1)
    local r2, q = decode(out, p)
    local r3 = decode(out, q)

    -- [1, msgid, error, result]: set_option -> void, no error
    assert.equal(1, r1[1])
    assert.equal(0, r1[2])
    assert.is_nil(r1[3]) -- error slot is nil

    -- get_option reflects the value set in the prior request
    assert.equal(1, r2[2])
    assert.is_nil(r2[3])
    assert.equal("test-model-x", r2[4])

    -- get_var reflects the var init.lua set via mua.g (same dispatch table)
    assert.equal(2, r3[2])
    assert.is_nil(r3[3])
    assert.equal("hi", r3[4])
  end)

  it("returns an error response for an unknown method", function()
    local out, code = run_embed(request(7, "mua_no_such_method", {}), FIX)
    assert.equal(0, code) -- a bad method is an addressable error, not a crash

    local r = decode(out, 1)
    assert.equal(1, r[1])
    assert.equal(7, r[2])
    assert.equal("table", type(r[3])) -- error = [code, message]
    assert.truthy(r[3][2]:find("unknown method", 1, true))
    assert.is_nil(r[4]) -- no result
  end)
end)
