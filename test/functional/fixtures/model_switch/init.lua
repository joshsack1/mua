-- Model-switch fixture for the functional model_switch_spec. Sets a launch
-- model and one UserPromptPre hook that reassigns mua.o.model so a later turn
-- uses a different model:
--   * "/model B"       -> switch to "model-b" and swallow the turn (REPL use),
--   * "switch-and-run" -> switch to "model-b" and let the turn run (-p use),
--   * anything else    -> pass through unchanged on the launch model.
mua.o.model = "model-a"

mua.create_autocmd("UserPromptPre", {
  callback = function(ev)
    if ev.prompt == "/model B" then
      mua.o.model = "model-b"
      return false -- swallow: just switch, run no turn
    end
    if ev.prompt == "switch-and-run" then
      mua.o.model = "model-b"
      return nil -- switch, then let this turn run on the new model
    end
    return nil -- allow unchanged on the launch model
  end,
})
