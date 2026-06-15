local helpers = require("test.functional.helpers")

-- Options set in init.lua flow through the mua.api bridge into the C store.
-- These exercise the bridge end to end without a server: the REPL evaluates
-- init.lua at startup, then EOF on stdin quits with exit 0. init.lua print()
-- lands on stdout; a raised error lands on stderr (nonfatal, nvim-style).
-- The behavioral effect of each option on a request is covered separately
-- once the agent reads the store.
local function opts_env(config_dir)
  return {
    MUA_CONFIG_DIR = config_dir,
    OPENROUTER_API_KEY = "test-key", -- required to reach the REPL; never used (EOF quits)
    MUA_STATE_DIR = helpers.tmpdir(),
    MUA_LOG = "",
  }
end

describe("mua.o options bridge", function()
  it("sets and reads back options of each type through mua.o", function()
    local r = helpers.run_mua({}, opts_env("test/functional/fixtures/opts_load"), { stdin = "" })
    assert.equal(0, r.code)
    assert.truthy(r.stdout:find("STEPCAP=7", 1, true)) -- integer round-trip
    assert.truthy(r.stdout:find("MODEL=demo/model", 1, true)) -- string round-trip
    assert.truthy(r.stdout:find("SP1=You", 1, true)) -- system_prompt round-trip
  end)

  it("reports an unknown option but stays nonfatal", function()
    local r = helpers.run_mua({}, opts_env("test/functional/fixtures/opts_unknown"), { stdin = "" })
    assert.equal(0, r.code)
    assert.truthy(r.stderr:find("init.lua", 1, true))
    assert.truthy(r.stderr:find("bogus", 1, true))
  end)
end)
