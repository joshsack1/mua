# mua.api + Lua bridge — design

**Status: in progress.** Delivered so far: the `mua.o` options bridge (the global API seam
`mua_set_option`/`mua_get_option` in [global.c](../src/mua/api/global.c) annotated
`FUNC_API_SINCE(1)`, the table-driven `mua.api.mua_*` registration in
[bridge.c](../src/mua/lua/bridge.c), the C options store in [options.c](../src/mua/options.c),
and the `mua.o` sugar in [runtime/lua/mua/init.lua](../runtime/lua/mua/init.lua)); and
**Step 1 below** — Lua↔Object value marshaling and `mua.g`. This file is the design record for
the milestone — the sequence that takes `mua.api` from "options only" to the full neovim-style
surface (user tools, autocmd hooks, session scope, and an `--embed` RPC frontend).

These steps wrap the agent; they never bypass it. The load-bearing contracts they bind are
the "Agent invariants" under Architecture in [CLAUDE.md](../CLAUDE.md) (the full design
record for those is [agent-milestone.md](agent-milestone.md)). The two that matter most here:
**#4** — tool failures are `ToolResult{is_error}`, never `Error *`; the registry shape
(name/description/schema/callback) is the seed of `mua_register_tool` — and **#5** — the
agent gate is a single function-pointer chokepoint the Lua `ToolPre` hook wraps.

## Foundation (baseline at the milestone's start)

What the sequence built on, captured before Step 1 — which has since added the Object↔Lua
marshaling and the shared `api_copy_object`/`api_free_object` that the "scalars only" bullet
below predates:

- **API types** ([defs.h](../src/mua/api/private/defs.h)): the `Object` tagged union already
  carries `kObjectTypeArray`/`kObjectTypeDict` alongside the scalars, `Array`/`Dict` structs
  are defined, and `Session` is a `handle_T` (`int32_t`) with `0` documented as "current".
  `FUNC_API_SINCE(x)` is a no-op macro today — a forward slot for a dispatch generator.
- **Lua marshaling is scalars only**: `lua_to_object` in [bridge.c](../src/mua/lua/bridge.c)
  handles string/number/boolean/nil. There is **no** `object_to_lua`, and no table↔Array/Dict
  path. This is the gating gap for everything that passes a structured value.
- **Tools**: a static `tool_defs[]` of `ToolDef{name, description, params_schema, mutating,
  execute}` in [tools.c](../src/mua/tools.c); `tools_lookup` and `tools_build_openai_array`
  iterate it. The execute contract (`ToolExecuteFn(cJSON *args, ToolDoneCb done, void *ud)`,
  [tools.h](../src/mua/tools.h)) is uniform: sync tools fire `done` inline and return `NULL`;
  async tools return a cancellable handle. Failures are `ToolResult{content, is_error}`.
- **Gate + callbacks** ([agent.h](../src/mua/agent.h)): `AgentCallbacks` holds `on_text`,
  `on_tool_start`, `on_tool_result`, `on_finish`, and the `AgentGateFn gate` chokepoint
  (`(ud, tool, args, char **refusal) -> GateDecision`). The agent fires these at fixed points
  in [agent.c](../src/mua/agent.c) — they are the exact seams the autocmd events attach to.
- **Session**: `SessionState` owns the conversation as a borrowable cJSON array;
  `session_messages`/`session_message_get`/`session_id` in [session.c](../src/mua/session.c)
  read it. The run uses one implicit current session (`setup_session`…`session_free` in
  [main.c](../src/mua/main.c)); there is no handle→pointer table yet.
- **RPC seam**: the `luaL_Reg api_functions[]` table in [bridge.c](../src/mua/lua/bridge.c)
  holds `lua_CFunction` pointers — meaningless to a non-Lua frontend. A shared metadata layer
  has to come first; the comment in [global.c](../src/mua/api/global.c) already names "an
  `--embed` RPC dispatcher" as the API layer's second consumer.

### Three marshaling axes (a recurring subtlety)

The bridge converts between *three* representations, and conflating them is the easy mistake:

1. **Object ↔ Lua** — the API type system to/from Lua values (`mua.o`, `mua.g`, scalar args).
2. **cJSON ↔ Lua** — wire JSON to/from Lua: tool `arguments`, tool results, a tool's schema,
   and session messages are all wire-shaped cJSON, *not* `Object`.
3. **Object ↔ msgpack** — only for `--embed` (Step 5).

Each is a bounded-depth tree walk over model- or user-controlled structure, so each needs an
explicit depth cap (code-safety: no unbounded recursion on input-driven depth) and must treat
`String` as not NUL-terminated (length-prefixed `lua_tolstring`/`lua_pushlstring`).

## Sequence to done

Dependency-ordered. Step 1 is the cross-cutting unblocker; 2–4 each consume it; 5 is the
capstone and is mostly greenfield.

### Step 1 — Value marshaling + `mua.g` — ✅ Landed

**Landed** (`f853f74`, `8cddc25`, `95401de`, `b2a72c2`). What shipped, and where it deviated
from the design recorded below:

- The marshaling lives in a **separate** owned popper `lua_pop_object` (arena-backed) plus
  `object_to_lua`, rather than *extending* the borrowing scalar `lua_to_object` — so the tested
  options path stayed byte-for-byte untouched.
- A per-call `Arena` holds the build tree, so a mid-walk rejection at any depth frees with a
  single `arena_finish` before the `lua_error` longjmp — no partial-tree unwinding.
- The copy/free primitives were promoted to shared, **iterative** `api_copy_object` /
  `api_free_object` in [helpers.c](../src/mua/api/private/helpers.c) (explicit stack, no
  recursion — json.h's rule), and options.c was refactored onto them. The planned
  `array_grow`/`dict_grow` proved unnecessary: sizes are known from classification, so backing
  arrays are allocated exactly once.
- Both walks share one `kMarshalDepthCap` (32); on the Lua→Object side it is also the cycle
  guard. Tables classify as Array (keys 1..n), Dict (string keys), or empty→Array.

The original design follows.

- **Goal**: arbitrary-value globals from Lua (`mua.g.x = {...}`, the `vim.g` analog) — and the
  Array/Dict ↔ Lua marshaling every later step needs.
- **Why first**: it is the cross-cutting unblocker (register_tool's schema/args,
  `mua_sess_get_messages`' return, richer hook payloads). `mua.g` is the lowest-risk surface
  to land it behind, mirroring `mua.o` exactly. The options slice deliberately deferred `mua.g`
  rather than ship a scalar-only stub — so doing it right *means* table support, which *means*
  this marshaling.
- **Build**: add `object_to_lua` (the missing counterpart to `lua_to_object`); extend
  `lua_to_object` for `LUA_TTABLE` → Array when keys are a contiguous `1..n` run, else Dict
  (the nvim `nlua_push_Object`/`nlua_pop_Object` heuristic). A new open-ended string→Object
  store `src/mua/variables.c` — the fixed-schema `option_defs[]` in [options.c](../src/mua/options.c)
  won't do for arbitrary keys — documented as mutable singleton #4, with bounded growth and a
  `variables_free` reset hook. `mua_set_var`/`mua_get_var` in [global.c](../src/mua/api/global.c)
  (`FUNC_API_SINCE(2)`). `mua.g` sugar in [runtime/lua/mua/init.lua](../runtime/lua/mua/init.lua),
  the same `setmetatable` forwarding as `mua.o`.
- **Pitfalls**: bounded recursion depth on the table walk (explicit cap, the JSON parse-depth
  limit style); `String` is not NUL-terminated; reuse the longjmp-safe `raise_api_error` free
  pattern (free the C `Error` *before* `lua_error`).
- **Tests**: `mua.g` nested-table round-trip; an over-deep table rejected at the cap;
  `variables_free` in the FFI spec's `before_each` (one busted process shares one copy of the
  store); functional — `init.lua` sets `mua.g`, a later read sees it.

### Step 2 — `mua_register_tool`

- **Goal**: user-defined tools from Lua (`mua.api.mua_register_tool{name=, description=,
  schema=, callback=}`) — the `nvim_create_user_command` analog, and invariant #4 realized.
- **Build**: promote the registry from the static `tool_defs[]` to static built-ins **plus** a
  dynamic registered list (capped count); `tools_lookup` and `tools_build_openai_array` iterate
  both. A C `ToolExecuteFn` shim marshals the cJSON `args` → Lua, calls the stored callback
  (held via `luaL_ref`), marshals the returned Lua value → `ToolResult{content, is_error}`, and
  fires `done` **inline** (Lua is synchronous → return `NULL`, the sync half of the contract).
  Store a registered tool's schema as a cJSON tree on the dynamic entry (built-ins keep their
  static `params_schema` string); `tools_build_openai_array` branches on which it holds.
- **Pitfalls**: this is the cJSON↔Lua axis, distinct from Step 1's Object↔Lua — reuse the
  depth-capped walk but against cJSON. A callback that errors must become
  `ToolResult{is_error=true}` (wrap the call in `lua_pcall`), never a `lua_error` thrown across
  the agent loop. The callback runs on the event-loop thread — fine for sync, but a slow Lua
  tool blocks the loop; document it.
- **Tests**: functional — `init.lua` registers a tool, the SSE fixture emits a `tool_call` for
  it, assert the callback ran, its result reached the next request, and the tool appears in the
  request `tools` array; a raising callback → `is_error` result, turn continues.

*(Loosely coupled with Step 3 and could swap with it. It comes first because hooks are most
useful once user tools exist, and because it forces the cJSON↔Lua args marshaling that Step 3's
`ToolPre`/`ToolPost` payloads reuse.)*

### Step 3 — Autocmd event set

- **Goal**: `mua.api.mua_create_autocmd("ToolPre", {callback = ...})` over the fixed event set
  `SessionStart`/`SessionEnd`/`ToolPre`/`ToolPost`/`StreamDelta` — invariant #5 realized.
- **Build**: an event enum and a bounded registration store (event → list of callback refs);
  `mua_create_autocmd`/`mua_clear_autocmds`. Dispatch at the C callback seams that **already
  exist** — this is wiring, not new control flow:
  - `StreamDelta` ← `provider_on_text` ([agent.c](../src/mua/agent.c)). Hot path: keep the
    no-handler case a single nil-check.
  - `ToolPre` ← the `AgentGateFn` chokepoint (`turn->cb.gate(...)` in
    [agent.c](../src/mua/agent.c)). The **only** veto-capable event: a hook returning false maps
    to `kGateRefuse` and a synthetic result. The `on_tool_start` callback is its notify side.
  - `ToolPost` ← the `on_tool_result` call ([agent.c](../src/mua/agent.c)).
  - `SessionStart` ← after `setup_session`; `SessionEnd` ← before `session_free`
    (both in [main.c](../src/mua/main.c)).
- **Pitfalls**: record how `ToolPre` composes with the built-in interactive gate — recommended:
  Lua hooks run first, and a refusal short-circuits the y/N prompt. A throwing hook must be
  nonfatal (pcall, log, continue). `ToolPre`/`ToolPost` payloads carry the tool name and an args
  Dict → need Steps 1–2 marshaling.
- **Tests**: functional — a `ToolPost` hook fires once per tool; a `ToolPre` returning false
  refuses the tool (synthetic error, no execute); `SessionStart`/`SessionEnd` fire once each; a
  throwing hook is nonfatal.

### Step 4 — Session scope `mua_sess_*`

- **Goal**: a session-scoped read API — `mua_sess_get_messages(Session, Error *)`,
  `mua_sess_get_id(Session, Error *)` — with the `Session` handle and `0` = current, the
  `nvim_buf_*` analog. (The session module's vocabulary already presages this.)
- **Build**: a handle→`SessionState *` table; today there is one implicit session, so introduce
  the indirection where `0` resolves to the current run's session. `mua_sess_get_messages`
  returns the conversation as an Array, reusing Step 1's marshaling over `session_messages`
  ([session.c](../src/mua/session.c)). New file `src/mua/api/session.c` — the contract's
  one-file-per-scope layout (`global.c`/`session.c`/`tools.c`). Read-only this slice: no
  append-from-Lua, which would risk the borrow-lifetime invariant.
- **Pitfalls**: borrow lifetime — `session_messages` is valid only until the next append;
  marshaling to a fresh Lua table copies, so the returned value is safe, but document that a
  caller must not cache a session pointer/handle across turns. `0` = current must raise a clean
  Validation error when there is no current session.
- **Tests**: unit — handle `0` resolves, a bad handle → Validation error; functional — a
  tool or hook calls `mua_sess_get_messages(0)` and sees the conversation so far.

### Step 5 — `--embed` msgpack-RPC server

- **Goal**: drive mua from an external process over msgpack-RPC on stdio — the nvim `--embed`
  analog; the capstone that proves `mua.api` is a real surface, not just Lua glue.
- **Build**: first the prerequisite — today's `api_functions[]` table is `lua_CFunction`-specific
  and not reusable by RPC. Introduce a shared API-metadata table (`{name, fn-ptr, arg/return
  Object signature}`) that **both** the Lua bridge and the RPC dispatcher consume — what
  `FUNC_API_SINCE` was always feeding. Then msgpack encode/decode (Object↔msgpack — mechanical
  and total, since `Object` already has Array/Dict), a stdio channel with
  request/response/notification framing, and the `--embed` CLI flag.
- **Pitfalls**: the largest, greenfield step. Do the shared-metadata refactor *first* so it
  retro-simplifies the Lua bridge (the per-function `l_mua_*` wrappers collapse into one
  table-driven dispatch). Bound message size and decode depth on the RPC read loop — it is
  untrusted, unbounded input.
- **Tests**: functional — spawn `mua --embed`, send a msgpack `mua_set_option`/`mua_get_option`
  request, assert the response; confirm the one metadata table drives both Lua and RPC.

## Out of scope (deferred past this milestone)

Append-from-Lua to a session (`mua_sess_append`) and tool *cancellation* from Lua both touch
the borrow/lifetime invariants and wait until the read-only surface above is settled. The async
half of the tool contract is unused by Lua tools (they are synchronous); a future Lua tool that
needs to await I/O would model it on the libuv handle pattern the `bash` tool already uses.
