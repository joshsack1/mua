# mua

A minimal coding agent in the style of neovim: a C core embedding LuaJIT,
configured and extended in Lua.

**Status:** C infrastructure scaffolded — `mua -p "prompt"` streams a completion
from OpenRouter; the agent loop, tools, sessions, and the `mua.api` surface are
still to come. See [CLAUDE.md](CLAUDE.md) for the founding spec.

## Building and testing

```sh
make                  # configure + build → build/bin/mua
make test             # unit (busted + LuaJIT FFI) and functional suites
make lint             # clang-tidy + luacheck
make format           # clang-format + stylua
TEST_FILE=test/functional/startup_spec.lua make functionaltest
make SANITIZE=1 BUILD_DIR=build-san test   # the suites under ASan+UBSan
```

## Usage

```sh
export OPENROUTER_API_KEY=sk-or-...
build/bin/mua -p "Reply with exactly: hello"
```

Configuration is plain Lua at `~/.config/mua/init.lua` (override the location
with `MUA_CONFIG_DIR`). The default model is `anthropic/claude-sonnet-4.6`;
`OPENROUTER_BASE_URL` overrides the API endpoint. `MUA_LOG=debug|info|warn|error`
enables trace logging on stderr. Exit codes: 0 success, 1 runtime/API failure,
64 usage error, 130 interrupted.

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
