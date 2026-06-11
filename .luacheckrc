std = "luajit"
cache = true
max_line_length = false -- line width is stylua's job
exclude_files = { "build", "test/functional/fixtures/broken" }

files["test/**/*_spec.lua"] = { std = "+busted" }
