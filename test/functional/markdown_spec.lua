local helpers = require("test.functional.helpers")

-- The markdown renderer is gated on `mua.o.markdown && isatty(stdout)`. With the
-- option enabled (via the fixture init.lua) but stdout captured through a pipe,
-- the isatty() half of the gate must keep output plain -- byte-identical to the
-- unrendered path, so every other functional spec stays valid. Rendering only
-- fires on a real TTY, which the io.popen harness does not provide; styling
-- correctness is owned by the unit suite (test/unit/render_spec.lua).
describe("markdown rendering gate", function()
  it("leaves piped output unstyled even when mua.o.markdown is enabled", function()
    local wire = helpers.text_block("**bold** and `code`")
    local srv = helpers.start_sse_server({ { { "send", wire }, { "close" } } })

    local env = helpers.mua_env(srv)
    env.MUA_CONFIG_DIR = "test/functional/fixtures/markdown_config"

    local r = helpers.run_mua({ "-p", "hi" }, env)
    local s = srv.finish()

    assert.equal(0, r.code)
    assert.equal("**bold** and `code`\n", r.stdout) -- literal markup, cosmetic newline
    assert.is_nil(r.stdout:find("\27", 1, true)) -- no ANSI escape (ESC) bytes
    assert.equal("", r.stderr) -- init.lua set the option cleanly
    assert.equal(0, s.code)
    assert.equal(1, #s.requests)
  end)
end)
