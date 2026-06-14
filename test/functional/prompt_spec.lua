local helpers = require("test.functional.helpers")

describe("mua -p", function()
  local key = os.getenv("OPENROUTER_API_KEY")

  it("usage error when -p has no argument", function()
    local r = helpers.run_mua({ "-p" })
    assert.equal(64, r.code)
    assert.truthy(r.stderr:match("missing argument"))
  end)

  it("fails with a clear message when no api key is set", function()
    local r = helpers.run_mua({ "-p", "hi" }, {
      OPENROUTER_API_KEY = "",
      MUA_CONFIG_DIR = "test/functional/fixtures/nonexistent",
      MUA_STATE_DIR = helpers.tmpdir(),
    })
    assert.equal(1, r.code)
    assert.truthy(r.stderr:match("OPENROUTER_API_KEY"))
  end)

  if key == nil or key == "" then
    it("streams a completion (live)", function()
      pending("set OPENROUTER_API_KEY to run the live streaming spec")
    end)
    return
  end

  it("streams a completion (live)", function()
    local r = helpers.run_mua({ "-p", "Reply with exactly: hello" }, {
      MUA_CONFIG_DIR = "test/functional/fixtures/nonexistent",
      MUA_STATE_DIR = helpers.tmpdir(),
    })
    assert.equal(0, r.code)
    assert.truthy(r.stdout:lower():match("hello"))
  end)

  it("reports the api error envelope for a bad key (live)", function()
    local r = helpers.run_mua({ "-p", "hi" }, {
      OPENROUTER_API_KEY = "sk-or-v1-invalid",
      MUA_CONFIG_DIR = "test/functional/fixtures/nonexistent",
      MUA_STATE_DIR = helpers.tmpdir(),
    })
    assert.equal(1, r.code)
    assert.truthy(r.stderr:match("api error"))
  end)
end)
