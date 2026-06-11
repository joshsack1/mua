std = "luajit"
cache = true
max_line_length = false -- line width is stylua's job
exclude_files = { "build" }

files["test/**/*_spec.lua"] = { std = "+busted" }
