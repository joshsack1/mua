local helpers = require("test.functional.helpers")

-- Bare `mua` enters the REPL, which evaluates init.lua at startup (before the
-- banner) and then reads stdin. Empty stdin (EOF) quits cleanly with no turn,
-- so these exercise init.lua evaluation without a server. The banner/prompt
-- are on stderr; init.lua's print() output is on stdout.
local function startup_env(config_dir)
  return {
    MUA_CONFIG_DIR = config_dir,
    OPENROUTER_API_KEY = "test-key", -- required to reach the REPL; never used (EOF quits)
    MUA_STATE_DIR = helpers.tmpdir(),
    MUA_LOG = "",
  }
end

describe("startup", function()
  it("evaluates init.lua from MUA_CONFIG_DIR and resolves runtime modules", function()
    local r = helpers.run_mua({}, startup_env("test/functional/fixtures/hello"), { stdin = "" })
    assert.equal(0, r.code)
    assert.truthy(r.stdout:match("hello from init"))
    assert.truthy(r.stdout:match("table")) -- require("mua") found the runtime stub
  end)

  it("reports a broken init.lua on stderr but stays nonfatal", function()
    local r = helpers.run_mua({}, startup_env("test/functional/fixtures/broken"), { stdin = "" })
    assert.equal(0, r.code)
    assert.truthy(r.stderr:match("init%.lua"))
  end)

  it("starts cleanly when the config dir does not exist", function()
    local r = helpers.run_mua(
      {},
      startup_env("test/functional/fixtures/nonexistent"),
      { stdin = "" }
    )
    assert.equal(0, r.code)
    assert.equal("", r.stdout) -- no init.lua output; banner/prompt are on stderr
  end)
end)
