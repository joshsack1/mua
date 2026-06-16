# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## What mua is

mua is a minimal coding agent — the feature class of [zot](https://github.com/patriceckhart/zot) and [pi](https://github.com/earendil-works/pi/tree/main/packages/coding-agent): an agent loop, four built-in tools (read/write/edit/bash), an LLM provider, persisted sessions — built in the style of [neovim](https://github.com/neovim/neovim): a C core embedding LuaJIT, with all configuration and extension done in Lua. The differentiator is the API surface: the exposed C API must feel immediately familiar to anyone who has spent serious time with neovim's `src/nvim/api/` or `vim.api`.

**Current state: a working agent.** mua runs as an interactive line-based REPL or a one-shot `mua -p "prompt"`, streaming from OpenRouter (with bounded retries and streamed tool-call accumulation), executing the four built-in tools (`read`/`write`/`edit`/`bash`) behind a y/N approval gate on the mutating ones, and persisting every turn as append-only JSONL under `~/.local/state/mua/sessions/` with `--resume` to reload the latest. The whole stack is covered by unit specs (busted + LuaJIT FFI) and functional specs that drive the binary against a scriptable SSE fixture server — `test/functional/fixtures/sse_server.c` — over real sockets, no API key needed; all of it runs clean under ASan/UBSan. **The `mua.api` + Lua bridge has begun:** `init.lua` now configures the agent through `mua.o` — the `vim.o`-style options proxy (`system_prompt`, `model`, `step_cap`) — backed by the first real `mua.api` functions (`mua_set_option`/`mua_get_option`); user-defined tools, autocmd hooks, and the rest of the surface are still ahead (Roadmap below). Keep the Commands section in sync with what actually exists — never list a target here that doesn't run.

## Roadmap

Next up (the agent milestone is done — see "Agent invariants" under Architecture; the `mua.api` bridge's options slice and Steps 1–4 — value marshaling + `mua.g`, `mua_register_tool`, the autocmd event set, and the session scope `mua_sess_*` — have landed):

1. **`mua.api` + Lua bridge (continued)** — the identity feature. *Landed:* the global API seam (`src/mua/api/global.c` with `FUNC_API_SINCE`, the table-driven `mua.api.mua_*` registration in `src/mua/lua/bridge.c`, the C-side options store in `src/mua/options.c`, the `mua.o` options sugar in `runtime/lua/mua/`), **Step 1** — Lua↔Object value marshaling (`object_to_lua`/`lua_pop_object`, shared `api_copy_object`/`api_free_object`) with the `mua.g` variable store (`src/mua/variables.c`) and sugar; **Step 2** — `mua_register_tool` (user-defined tools from Lua) with the `cjson_to_object`/`object_to_cjson` converter (`src/mua/json.c`) and the dynamic registry (`src/mua/tools.c`); **Step 3** — the autocmd event set (`mua_create_autocmd`) with the core store (`src/mua/autocmd.c`), dispatched from main.c at the agent's existing callback seams; and **Step 4** — the session scope `mua_sess_*` (`mua_sess_get_messages`/`mua_sess_get_id`, `src/mua/api/session.c`) over a current-session registry (`session_resolve`, `0` = current). *Sequence to done* (dependency-ordered; full breakdown in [docs/api-bridge.md](docs/api-bridge.md)):
   1. ✅ **Value marshaling + `mua.g`** — *landed* (`f853f74`..`b2a72c2`): Array/Dict ↔ Lua via a bounded-depth walk backing an open-ended variable store; `mua.g` (the `vim.g` analog) is its first consumer.
   2. ✅ **`mua_register_tool`** — *landed* (`bfb7015`..`defcac3`): a dynamic tool registry beside the static `tool_defs[]`; the Lua callback (a `LuaRef`, held via `luaL_ref`) wrapped as a synchronous `ToolExecuteFn` shim that fires `done` inline. Marshals cJSON↔Object↔Lua (the `cjson_to_object`/`object_to_cjson` converter feeds Step 1's Object↔Lua, so cJSON never enters the bridge); gated by default. The `nvim_create_user_command` analog.
   3. ✅ **Autocmd event set** — *landed* (`b76a18f`..`70c00af`): `mua_create_autocmd` over `SessionStart`/`SessionEnd`/`ToolPre`/`ToolPost`/`StreamDelta`, dispatched from main.c at the existing callback seams (the agent loop stays Lua-free). `ToolPre` fires before **every** tool and can veto — the gate became the all-tool chokepoint, with the mutating-only y/N moved into the policies. The `nvim_create_autocmd` analog.
   4. ✅ **Session scope `mua_sess_*`** — *landed* (`4379ab8`..`8e85598`): the first integer-handle scope (the `nvim_buf_*` analog) — `mua_sess_get_messages`/`mua_sess_get_id`, `0` = current — over a current-session registry (`session_resolve`/`session_set_current` in `src/mua/session.c`, mutable singleton #6), not yet a full handle table. Read-only; `get_messages` marshals the conversation cJSON→Object at the one boundary place, so the bridge stays cJSON-free.
   5. **`--embed` msgpack-RPC server** — a shared API-metadata table feeding both the Lua bridge and an RPC dispatcher (the Lua `luaL_Reg` table is not reusable as-is), then msgpack over stdio.

Housekeeping, slotted whenever: CI running the full gauntlet (`make && make test && make lint` plus the `SANITIZE=1` suites); a LICENSE decision before the repo goes public (none exists yet, deliberately; Apache-2.0 would match neovim); a `MUA_HTTP_STALL_WINDOW_S` env override so stall detection becomes functionally testable against the fixture server; surface OpenRouter's `error.metadata.raw` in the provider's error message (`report_http_failure` in [openrouter.c](src/mua/provider/openrouter.c) prints only the top-level `error.message`, which is often a useless `"Provider returned error"` — the real upstream cause and the routing/provider detail live in `metadata.raw`, e.g. `"messages: at least one message is required"`; discarding it turned a one-line request-shape bug into a long diagnosis).

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
TEST_FILE=test/functional/startup_spec.lua make functionaltest   # run a single spec file
make SANITIZE=1 BUILD_DIR=build-san test   # the suites under ASan+UBSan (second gear)
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

Configuration precedence (settled with the options bridge): environment variables are per-invocation overrides and **always win**; `mua.o` in `init.lua` sets standing defaults below them; the built-in is the last resort. So `system_prompt` and `step_cap` resolve env > `mua.o` > built-in default; `model` has no env var, so it resolves CLI `-m` > `mua.o` > provider default. An empty `system_prompt` at either explicit layer omits the system message. The options store ([options.c](src/mua/options.c)) is the third documented mutable singleton, after the event loop and the Lua state.

Dependencies are deliberately few: **LuaJIT** (Lua 5.1 fallback), **libuv** (event loop — same as neovim), **libcurl** (HTTPS + SSE streaming), and a small vendored JSON library. Resist adding more.

Core runtime flow: startup evaluates `init.lua` → user submits a prompt → agent loop builds the request from the session, streams a response from the provider (OpenRouter's OpenAI-compatible Chat Completions API first, `OPENROUTER_API_KEY`; keep the provider interface thin enough to add others later), executes any tool calls, appends results to the session, repeats until the model stops or the **hard step cap** (default 50) is hit. Sessions persist as append-only JSONL, one message per line, under `~/.local/state/mua/sessions/` (pi-style). XDG throughout: config in `~/.config/mua`, state in `~/.local/state/mua`.

Tools: the four built-ins (`read`, `write`, `edit` by exact-match replacement, `bash` with a mandatory timeout) are implemented in C. User-defined tools are registered from Lua via `mua.api.mua_register_tool` — the analog of `nvim_create_user_command`. Lifecycle hooks are autocmd-shaped: `mua.api.mua_create_autocmd("ToolPre", { callback = ... })` over a small fixed event set (`SessionStart`, `SessionEnd`, `ToolPre`, `ToolPost`, `StreamDelta`).

### Agent invariants (settled — the API layer wraps these, never bypasses them)

These held the agent milestone together and bind the `mua.api` bridge; the full design record is [docs/agent-milestone.md](docs/agent-milestone.md).

1. Messages are wire-shaped `cJSON`; the session owns the conversation array; build request bodies zero-copy with `cJSON_CreateArrayReference(messages->child)` (the array's first element, not the array node — verified against vendored cJSON 1.7.18). Adding existing messages to a request array uses `cJSON_AddItemReferenceToArray`, never `cJSON_CreateObjectReference` (which nests the message under an empty key and drops its role).
2. `session_append(SessionState *, cJSON *msg, Error *)` takes ownership of `msg` unconditionally (frees even on failure); appended nodes stay borrowable until `session_free`.
3. Provider `on_done(ud, cJSON *message, finish_reason, tokens)` transfers ownership of the assembled wire-shaped assistant message; the `messages`/`tools` opts are borrowed only for the `openrouter_stream` call. The agent issues each request from a 0-ms timer, never inline from a provider callback (`curl_multi_add_handle` inside curl's own callback is `CURLM_RECURSIVE_API_CALL`).
4. Tool failures are `ToolResult{is_error}`, never `Error *`; one async execute contract (sync tools fire `done` inline); the registry shape (name/description/schema/callback) is the seed of `mua_register_tool`.
5. The agent gate is a single function-pointer chokepoint (the future Lua `ToolPre` hook wraps it); a dangling `tool_calls` message must always be answered with results (synthetic ones on refuse/cancel/resume) or every resumed request 400s.
6. Tool-call detection: a nonempty `tool_calls` array is authoritative over the `finish_reason` string; the 50-step cap stops the turn, never loops.

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
