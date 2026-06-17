-- UserPromptPre fixture for the functional userprompt_spec. One hook that
-- swallows the prompt "/skip" (return false -> no turn runs), rewrites the
-- prompt "rewriteme" to "REWRITTEN-PROMPT" (string return), and lets anything
-- else through unchanged (nil).
mua.create_autocmd("UserPromptPre", {
  callback = function(ev)
    if ev.prompt == "/skip" then
      return false -- swallow: skip the turn
    end
    if ev.prompt == "rewriteme" then
      return "REWRITTEN-PROMPT" -- rewrite before the turn is built
    end
    return nil -- allow unchanged
  end,
})
