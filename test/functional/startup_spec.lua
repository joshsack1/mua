local helpers = require("test.functional.helpers")

describe("startup", function()
  it("evaluates init.lua from MUA_CONFIG_DIR and resolves runtime modules", function()
    local r = helpers.run_mua({}, { MUA_CONFIG_DIR = "test/functional/fixtures/hello" })
    assert.equal(0, r.code)
    assert.truthy(r.stdout:match("hello from init"))
    assert.truthy(r.stdout:match("table")) -- require("mua") found the runtime stub
    assert.equal("", r.stderr)
  end)

  it("reports a broken init.lua on stderr but stays nonfatal", function()
    local r = helpers.run_mua({}, { MUA_CONFIG_DIR = "test/functional/fixtures/broken" })
    assert.equal(0, r.code)
    assert.truthy(r.stderr:match("init%.lua"))
  end)

  it("is silent when the config dir does not exist", function()
    local r = helpers.run_mua({}, { MUA_CONFIG_DIR = "test/functional/fixtures/nonexistent" })
    assert.equal(0, r.code)
    assert.equal("", r.stdout)
    assert.equal("", r.stderr)
  end)
end)
