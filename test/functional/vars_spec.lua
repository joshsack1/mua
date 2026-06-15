local helpers = require("test.functional.helpers")

-- mua.g values set in init.lua flow through the mua.api bridge and the value
-- marshaling into the C store, then read back rebuilt. These drive the bridge
-- end to end without a server: the REPL evaluates init.lua at startup, then EOF
-- on stdin quits with exit 0. print() lands on stdout; a raised error (an
-- unmarshalable value) lands on stderr, nonfatal -- the nvim-style behavior the
-- startup and opts specs also assert. This is the only coverage of the static
-- lua_pop_object/object_to_lua marshaling, which FFI cannot reach directly.
local function vars_env(config_dir)
  return {
    MUA_CONFIG_DIR = config_dir,
    OPENROUTER_API_KEY = "test-key", -- required to reach the REPL; never used (EOF quits)
    MUA_STATE_DIR = helpers.tmpdir(),
    MUA_LOG = "",
  }
end

describe("mua.g value marshaling", function()
  it("round-trips scalars and nested tables", function()
    local r = helpers.run_mua({}, vars_env("test/functional/fixtures/vars_load"), { stdin = "" })
    assert.equal(0, r.code)
    assert.truthy(r.stdout:find("COUNT=7", 1, true)) -- integer
    assert.truthy(r.stdout:find("NAME=demo", 1, true)) -- string
    assert.truthy(r.stdout:find("A2=2", 1, true)) -- nested array element
    assert.truthy(r.stdout:find("LEN=3", 1, true)) -- array length preserved
    assert.truthy(r.stdout:find("FLAG=true", 1, true)) -- nested boolean
    assert.truthy(r.stdout:find("TEMP=nil", 1, true)) -- nil deleted the key
    assert.truthy(r.stdout:find("UNSET=nil", 1, true)) -- unset reads as nil
  end)

  it("reports a table with mixed keys but stays nonfatal", function()
    local r = helpers.run_mua({}, vars_env("test/functional/fixtures/vars_mixed"), { stdin = "" })
    assert.equal(0, r.code)
    assert.truthy(r.stderr:find("init.lua", 1, true))
    assert.truthy(r.stderr:find("mixed", 1, true))
    assert.is_nil(r.stdout:find("UNREACHED", 1, true))
  end)

  it("reports a cyclic table at the depth cap but stays nonfatal", function()
    local r = helpers.run_mua({}, vars_env("test/functional/fixtures/vars_cycle"), { stdin = "" })
    assert.equal(0, r.code)
    assert.truthy(r.stderr:find("init.lua", 1, true))
    assert.truthy(r.stderr:find("nesting exceeds", 1, true))
    assert.is_nil(r.stdout:find("UNREACHED", 1, true))
  end)
end)
