std = "luajit"
cache = true
max_line_length = false -- line width is stylua's job
exclude_files = { "build", "test/functional/fixtures/broken" }

files["test/**/*_spec.lua"] = { std = "+busted" }

-- init.lua (user config, and these fixtures) runs with the C-injected `mua`
-- global -- mua's analog of neovim's `vim` -- which it both reads and mutates.
files["test/functional/fixtures/**/init.lua"] = { globals = { "mua" } }
