# mua

A minimal coding agent in the style of neovim: a C core embedding LuaJIT,
configured and extended in Lua.

**Status:** a working coding agent — an interactive REPL or a one-shot
`mua -p`, four built-in tools (read/write/edit/bash) behind an approval gate,
and resumable JSONL sessions, all streaming from OpenRouter. The Lua
extension surface (`mua.api`) is still to come. See [CLAUDE.md](CLAUDE.md) for
the founding spec.

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

build/bin/mua                       # interactive REPL ('exit' or Ctrl-D to quit)
build/bin/mua -p "summarize README.md in one line"   # one-shot turn
build/bin/mua --resume -p "now do the same for CLAUDE.md"  # continue the latest session
```

The model's reply streams to stdout; the prompt, tool trace, and notices go to
stderr (so `mua -p … | tee out.txt` captures only the model). The model can
call the built-in `read`/`write`/`edit`/`bash` tools; mutating tools prompt for
`y/N` approval at the REPL. Flags:

- `-p, --prompt TEXT` — run a single turn instead of the REPL (mutating tools
  are auto-refused unless `--yes`).
- `-y, --yes` — approve gated tool calls without prompting.
- `-r, --resume` — reload the most recent session (none yet → notice, fresh start).
- `-m, --model ID` — override the model for this run.

Sessions are append-only JSONL under `~/.local/state/mua/sessions/`
(`MUA_STATE_DIR` overrides). Configuration is plain Lua at
`~/.config/mua/init.lua` (`MUA_CONFIG_DIR` overrides), evaluated at startup. It
can set agent options through `mua.o`, the neovim `vim.o`-style proxy:

```lua
-- ~/.config/mua/init.lua
mua.o.system_prompt = "You are a terse Rust reviewer." -- "" omits the system message
mua.o.model = "z-ai/glm-5.1"
mua.o.step_cap = 30
mua.o.markdown = true -- render markdown as ANSI on a TTY (default false)
```

Precedence is environment variable (where one exists) > `init.lua` > built-in
default; for `model`, the `-m` flag wins over `init.lua`. The default model is
`anthropic/claude-sonnet-4.6`; `OPENROUTER_BASE_URL` overrides the API endpoint.
With `mua.o.markdown` enabled, replies stream through a minimal markdown→ANSI
renderer (headings, bold/italic, inline and fenced code) only when stdout is a
terminal — piping (`mua -p … | tee`) stays byte-for-byte plain.
`MUA_LOG=debug|info|warn|error` enables trace logging on stderr. Exit codes: 0
success, 1 runtime/API failure, 64 usage error, 130 interrupted.

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

## License

mua is licensed under the [Apache License 2.0](LICENSE). It bundles
[cJSON](https://github.com/DaveGamble/cJSON) (MIT) under `src/cjson/`; see
[NOTICE](NOTICE).
