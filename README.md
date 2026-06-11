# mua

A minimal coding agent in the style of neovim: a C core embedding LuaJIT,
configured and extended in Lua.

**Status:** early scaffolding; see [CLAUDE.md](CLAUDE.md) for the founding spec.

## Bootstrap (macOS)

The build needs tools beyond Xcode's Command Line Tools. Homebrew installs may
require an administrator shell:

```sh
brew install cmake ninja clang-format llvm stylua luacheck luarocks
```

`llvm` is keg-only (~2 GB) and is the only source of clang-tidy; nothing else
from it is used. Do **not** `brew install curl` — mua links the macOS system
libcurl found through pkg-config.

The test runner is busted, installed through luarocks and paired with LuaJIT
(the unit tests need LuaJIT's `ffi` module):

```sh
luarocks --lua-version=5.1 --lua-dir=/opt/homebrew/opt/luajit install busted
```

If luarocks fails to find LuaJIT via `--lua-dir`, configure it explicitly:

```sh
luarocks config lua_version 5.1
luarocks config variables.LUA /opt/homebrew/bin/luajit
```

Verify: `busted --version` runs, and `head -1 "$(command -v busted)"` references
luajit.
