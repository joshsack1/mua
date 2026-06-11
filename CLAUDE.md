# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## What mua is

mua is a minimal coding agent — the feature class of [zot](https://github.com/patriceckhart/zot) and [pi](https://github.com/earendil-works/pi/tree/main/packages/coding-agent): an agent loop, four built-in tools (read/write/edit/bash), an LLM provider, persisted sessions — built in the style of [neovim](https://github.com/neovim/neovim): a C core embedding LuaJIT, with all configuration and extension done in Lua. The differentiator is the API surface: the exposed C API must feel immediately familiar to anyone who has spent serious time with neovim's `src/nvim/api/` or `vim.api`.

**Current state: greenfield.** The repo has no commits yet; this file is the founding spec. As scaffolding lands, keep the Commands section in sync with what actually exists — never list a target here that doesn't run.

## Hard rules

1. **All C code follows the code-safety skill.** Invoke the `anthropic-skills:code-safety` skill before writing or reviewing any C in this repo. The repo-specific distillation lives in "Code safety" below and applies even if the skill is unavailable.
2. **Commits never claim co-authorship.** No `Co-Authored-By:` trailers, no "Generated with Claude Code" footers, no other authorship or generation attribution in commit messages. This deliberately overrides Claude Code's default commit trailer. Commit messages follow neovim's conventional style: `feat(api): ...`, `fix(tools): ...`, `docs: ...`.

## Commands

Top-level Makefile wrapping CMake + Ninja, mirroring neovim's build interface. These targets are the contract — create them with the first scaffolding commit and keep them working:

```sh
make                  # configure + build → build/bin/mua
make test             # everything below
make unittest         # C-internal tests (busted + LuaJIT FFI, neovim test/unit style)
make functionaltest   # end-to-end Lua specs driving the built binary (test/functional/)
make lint             # clang-tidy on C, luacheck on Lua
make format           # clang-format on C, stylua on Lua
TEST_FILE=test/functional/agent_spec.lua make functionaltest   # run a single spec file
```

CMake never invoked directly except for debugging the build itself. Dependencies (LuaJIT, libuv, libcurl) are found on the system first; a `cmake.deps`-style bundled fallback can come later if needed.

## Architecture

Layering, top (user) to bottom (core):

```
~/.config/mua/init.lua     user config — plain Lua, evaluated at startup (like nvim's init.lua)
runtime/lua/mua/           shipped Lua stdlib: mua.o (options), mua.g (globals), sugar — all built on mua.api
src/mua/lua/               C↔Lua bridge: registers every C API function as mua.api.mua_* (1:1, like vim.api)
src/mua/api/               the public C API — the neovim-style surface (see contract below)
src/mua/                   core: agent loop, built-in tools, provider client, session store, event loop
```

Dependencies are deliberately few: **LuaJIT** (Lua 5.1 fallback), **libuv** (event loop — same as neovim), **libcurl** (HTTPS + SSE streaming), and a small vendored JSON library. Resist adding more.

Core runtime flow: startup evaluates `init.lua` → user submits a prompt → agent loop builds the request from the session, streams a response from the provider (OpenRouter's OpenAI-compatible Chat Completions API first, `OPENROUTER_API_KEY`; keep the provider interface thin enough to add others later), executes any tool calls, appends results to the session, repeats until the model stops or the **hard step cap** (default 50) is hit. Sessions persist as append-only JSONL, one message per line, under `~/.local/state/mua/sessions/` (pi-style). XDG throughout: config in `~/.config/mua`, state in `~/.local/state/mua`.

Tools: the four built-ins (`read`, `write`, `edit` by exact-match replacement, `bash` with a mandatory timeout) are implemented in C. User-defined tools are registered from Lua via `mua.api.mua_register_tool` — the analog of `nvim_create_user_command`. Lifecycle hooks are autocmd-shaped: `mua.api.mua_create_autocmd("ToolPre", { callback = ... })` over a small fixed event set (`SessionStart`, `SessionEnd`, `ToolPre`, `ToolPost`, `StreamDelta`).

## C API contract (the neovim part)

This is the project's reason to exist; hold the line on it.

- **Naming**: `mua_` prefix, scoped by handle exactly as nvim scopes by buffer/window: global functions are `mua_*` (`mua_register_tool`, `mua_create_autocmd`), session-scoped are `mua_sess_*` (`mua_sess_get_messages(Session sess, Error *err)`), tool-scoped `mua_tool_*`. Read `nvim_buf_get_lines` aloud; new names should scan the same way.
- **Handles** are integer ids (`Session`, etc.), like `Buffer`/`Window`/`Tabpage`. `0` means "current session", as `0` means current buffer in nvim.
- **API types** mirror `src/nvim/api/private/defs.h`: `String {char *data; size_t size}` (never assume NUL-termination), `Integer` (int64_t), `Boolean`, `Float`, `Array`, `Dict`, `Object`, plus the typed handles.
- **Errors**: every API function takes `Error *err` as its final parameter, set via `api_set_error(err, kErrorTypeValidation | kErrorTypeException, fmt, ...)`, checked with `ERROR_SET(err)`. Invalid input from a user or the model sets a Validation error and returns — it never asserts, aborts, or longjmps across the API boundary.
- **Memory**: internal allocation goes through `xmalloc`/`xcalloc`/`xrealloc`/`xstrdup` wrappers that abort on OOM and never return NULL (nvim's `memory.c` pattern). API return values are arena-allocated where practical; ownership is otherwise documented at the declaration.
- **Layout & versioning**: one file per scope in `src/mua/api/` (`global.c`, `session.c`, `tools.c`). Functions carry `FUNC_API_SINCE(n)` annotations. The API is append-only: deprecate, never repurpose or remove within a version.
- **Single source of truth**: the C API defines the surface; the bridge exposes it to Lua mechanically as `mua.api.mua_*` with identical names and argument order. Everything the Lua stdlib or user config can do must be reachable through `mua.api` — sugar wraps it, never bypasses it. Keep dispatch table-driven so an nvim-style `--embed` msgpack-RPC server can be added later without touching the API itself.

## Code safety (C distillation for this repo)

From the code-safety skill (NASA/JPL Power of Ten, adapted); these are merge requirements, not suggestions:

- **Zero warnings**: build with `-Wall -Wextra -Wpedantic -Werror`; clang-tidy clean. Rewrite code rather than suppressing a warning.
- **Bounded loops**: any loop driven by external input has an explicit cap — the agent loop's step limit, HTTP retries (capped, with backoff), SSE read loops, JSON parsing depth. Nothing the network or the model controls may loop unboundedly.
- **Check every return**: every call that can fail is checked. Intentionally discarded results are cast to `(void)` with a comment saying why.
- **Functions** stay single-purpose, soft ceiling ~60 lines. Narrowest possible scope for every declaration; no global mutable state beyond the explicit, documented singletons (event loop, session table).
- **Control flow**: no `goto` (a single `goto cleanup` for unwinding is the one tolerated exception, used sparingly); no recursion on input-driven depth — tree walks over model output use an explicit stack with a depth cap.
- **Assertions** for can't-happen invariants only; user/model input failures go through `Error *err`, never `assert`.
- **Memory growth**: this is a long-running process — every cache and accumulator has a bound; the xmalloc wrappers make OOM loud.
- **Preprocessor**: includes, include guards, and simple constants. No token pasting, no variadic macro tricks, no `#ifdef` thickets.
- **Pointers**: at most one level of dereference; no dereferences hidden behind macros or typedefs.

## Style

- C11. `.clang-format` checked in at the root; 2-space indent, neovim-flavored: snake_case functions, CamelCase types (`Error`, `String`, `Arena`).
- Lua formatted with stylua, linted with luacheck; both configs checked in.
- Functional tests (Lua/busted, `test/functional/`) are the primary coverage; FFI unit tests (`test/unit/`) where poking C internals directly earns its keep.
