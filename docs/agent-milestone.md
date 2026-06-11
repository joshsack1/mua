# Agent milestone — design

The detailed design for turning mua from a streaming client into an agent
(sessions, tool calling, the four built-in tools, the agent loop, a line-based
REPL). The ordered commit checklist and the load-bearing contracts live in
[CLAUDE.md](../CLAUDE.md) under "Agent milestone"; this file is the reference a
session reads before executing one of those commits. It is too large to land in
one context window — execute one or a few commits per session, ticking the
CLAUDE.md checklist as you go, each green on `make && make test && make lint`.

## Decisions locked with the user

- Mutating tools (write/edit/bash) require an interactive y/N at the REPL before
  executing; `read` runs automatically.
- Sessions persist as append-only JSONL **and** `--resume` reloads the latest —
  the round-trip is exercised from day one.
- Default provider OpenRouter (OpenAI-compatible), default model
  `anthropic/claude-sonnet-4.6`.

## The pivotal decision: messages are wire-shaped cJSON

A message is a `cJSON` object in exact OpenAI chat-completions shape; the session
owns one cJSON array that **is** the conversation. Request building is then
zero-conversion, a JSONL line is one printed message, and the assistant message
the provider assembles is appended verbatim — what the model sent is what the
file records is what the next request replays. This honors json.h's existing
boundary rule (cJSON flows between core modules, converts to `Object` only at the
future API layer). Native C message structs would need a tagged union plus two
serializers the moment `content` becomes an array of parts — rejected.

**Non-wire metadata via line-level record typing**, not envelopes or `x-`fields
(both reintroduce per-line conversion): a JSONL line is a *message record*
(object with string `"role"`) or a *control record* (object with string
`"type"`). v1 writes one control record — the header, line 1:
`{"type":"session","version":1,"id":...,"created":...}`. The loader tolerates
unknown `"type"` records (forward slot for future `{"type":"meta","usage":...}`).

**Implementation note (verified in vendored cJSON 1.7.18):**
`cJSON_CreateArrayReference(child)` sets the reference array's `->child` to the
passed node, so build the request body with
`cJSON_CreateArrayReference(messages->child)` — the array's **first element**,
not the array node (passing the array node yields `[[...]]`). `cJSON_Delete`
skips reference-node children (cJSON.c:259), so `json_free(body)` frees only the
reference wrappers, never the borrowed session trees. No `cJSON_Duplicate` (it
recurses).

## Modules

### `src/mua/paths.{c,h}` — add `paths_ensure_dir`

`bool paths_ensure_dir(const char *path, Error *err)` — mkdir -p semantics, mode
0700 (XDG state is private), EEXIST-tolerant, a non-directory in the way is a
Validation error, final component stat'd to confirm `S_ISDIR`. Iterates `'/'`
separators (bounded by strlen), every return checked. Reused by the session
store and the write tool.

### `src/mua/session.{c,h}` — append-only JSONL store

Vocabulary presages `mua_sess_*`. Surface (all take `SessionState *`; the
int-handle table with `0`=current is deferred to the API milestone — documented,
pure addition):

```c
SessionState *session_new(Error *err);            // mkdir sessions/, O_EXCL create, write header
SessionState *session_load(const char *path, Error *err);
SessionState *session_load_latest(Error *err);    // lexicographic max over fixed-width names; NULL+err if none
bool  session_append(SessionState *, cJSON *msg, Error *err);  // OWNS msg always (frees on failure too)
size_t       session_message_count(const SessionState *);
const cJSON *session_message_get(const SessionState *, size_t idx);  // borrowed, stable until free
const cJSON *session_messages(const SessionState *);                 // borrowed array for request build
const char  *session_id(const SessionState *);
void  session_free(SessionState *);
```

- **id/filename**: `YYYYMMDDTHHMMSS_NN.jsonl`, UTC, **fixed width always** (the
  `_NN` suffix makes plain `strcmp` order = creation order); collision via
  `O_CREAT|O_EXCL` bumping `NN` 00..99 (atomic, no TOCTOU). Discovery: single
  `readdir` pass keeping the strcmp-max in a fixed buffer, shape-filtered
  (rejects tmp/foreign names), bounded at 65536 entries.
- **append**: print → length-check (`MUA_SESSION_MAX_LINE` 8 MiB) → single
  `write(2)` of `[heal '\n'] + line + '\n'` → `cJSON_AddItemToArray`. **No
  fsync** (conversational state, not a WAL; torn-tail tolerance + heal byte cover
  OS-crash). A failed write poisons the session (latch; later appends fail fast).
- **load**: `O_RDWR|O_APPEND`, fstat size-latched chunked read (64 KiB, EINTR
  cap), memchr line scan into a capped Buf, `json_parse` per line, record-type
  dispatch. **Corrupt-line policy**: a bad line is staged; if another non-empty
  line follows → load fails naming the line (mid-file corruption desyncs the
  conversation); if EOF follows → tolerated torn tail (logged, skipped, healed on
  next append). Header must be line 1 with `version==1`. Caps: ≤4096 messages.

### `src/mua/provider/openrouter.{c,h}` — v2 tool calling

- **Opts**: replace `String prompt` with `const cJSON *messages` (REQUIRED
  non-empty array) + optional `const cJSON *tools` (OpenAI shape); both
  **borrowed only for the `openrouter_stream` call** (body printed synchronously;
  retries resend printed bytes). `tool_choice`/`parallel_tool_calls` omitted.
- **Streamed tool_calls accumulator**: fixed
  `OrToolCall tool_calls[MUA_MAX_TOOL_CALLS=16]` of
  `{present, char id[64], char name[128], Buf args(256 KiB)}`, plus
  `Buf content(1 MiB)` for the full assistant text (on_text still streams deltas
  live). Per `delta.tool_calls` item: `index` required (missing/negative/≥16 →
  terminal error — no silent drop), latch id/name from first fragment,
  concatenate `arguments` fragments per index; ≤64 items/event. Overflows are
  terminal errors, never truncation (a truncated assembled message corrupts the
  session).
- **New terminal callback**:
  `on_done(void *ud, cJSON *message, const String *finish_reason, int64_t completion_tokens)`
  — `message` is the COMPLETE wire-shaped assistant message (`role:"assistant"`,
  `content` string or `null`, optional `tool_calls[]` with
  `{id, type:"function", function:{name, arguments-string}}`), **ownership
  transferred**. Assembled in the existing `fire_done` chokepoint; incomplete
  calls (present but empty id/name) → on_error instead.
- **`delivered` now also set by any tool_call fragment** (not just content): one
  crisp "the model began answering" invariant; a mid-stream drop after partial
  accumulation fails fast rather than silently re-billing/re-generating.
- **Latent-bug fold-in**: `start_attempt` must reset *all* per-attempt state
  (finish_reason, completion_tokens, content Buf, every tool_calls entry, hwm) —
  today it resets only SSE/error-body, so a retry could inherit a latched
  finish_reason.
- **Test seam** (`openrouter_internal.h`): `new_for_test` initializes the Bufs;
  `free_for_test` frees them; add `openrouter_stream_delivered_for_test`. The FFI
  `on_done` harness prints+frees the handed-over message (pointer args only —
  LuaJIT-legal).

### `src/mua/tools.{c,h}` — registry + four builtins

```c
typedef struct { char *content; bool is_error; } ToolResult;       // content -> role:"tool" message verbatim
typedef void (*ToolDoneCb)(void *ud, ToolResult result);           // fires exactly once, maybe inline
typedef ToolExec *(*ToolExecuteFn)(cJSON *args, ToolDoneCb done, void *ud);  // NULL if done fired inline
typedef struct { const char *name, *description, *params_schema; bool mutating; ToolExecuteFn execute; } ToolDef;
const ToolDef *tools_lookup(const char *name);
cJSON *tools_build_openai_array(Error *err);   // fresh "tools":[...] per call; caller owns
void tools_cancel(ToolExec *exec);
```

One uniform async contract (sync tools call `done` inline) — the loop has one
continuation path, and it's the exact shape future `mua_register_tool` Lua tools
take. Tool **failures are results** (`is_error=true`), never `Error*` out of a
tool. Schemas are static strings parsed per request (no shared-cJSON cache).

- **read** `{path, offset?, limit?}` — regular files only; 256 KiB returned
  (head-truncated), 8 MiB scan bound; NUL byte → binary error (an embedded NUL
  would silently truncate the C string into cJSON); **no line-number prefixes**
  (would poison edit's exact match).
- **write** `{path, content}` — creates parent dirs via `paths_ensure_dir`;
  `O_TRUNC` + checked full write; returns bytes written.
- **edit** `{path, old_string, new_string}` — exact match, **0 occurrences →
  error, >1 → error with count**, no `replace_all` in v1; empty/identical
  old_string → error; 4 MiB file cap; in-place rewrite (preserves
  symlinks/links/owner; pre-edit content survives in the session log).
- **bash** `{command, timeout_ms?}` — `uv_spawn("/bin/sh","-c",command)`,
  stdin=/dev/null; timeout default 30 s, **clamped [1 ms, 120 s] hard max**
  regardless of model value; **merged stdout+stderr via one pipe** (`fds[1]`
  inherited onto both child fd 1 and 2 — true interleaving); 64 KiB output Buf,
  **drain-to-EOF past the cap** (stopping reads deadlocks the child on
  backpressure); SIGKILL on timeout; `done` fires only after exit_cb AND pipe EOF
  AND all uv_close callbacks, with a 500 ms post-exit grace timer for orphaned
  pipe holders. Result text carries output + truncation/timeout/exit
  annotations.

uv_spawn pitfall checklist (review gate): close child write-end after spawn or
EOF never arrives; `uv_pipe_open` owns fd0 (never manual close); child write-end
stays blocking; cap-hit drains never `uv_read_stop` early; exec struct freed in
the last of the three close callbacks; `uv_process_kill` after exit → `ESRCH` is
success; faithful exit-code/signal reporting.

### `src/mua/agent.{c,h}` — the loop

```c
typedef enum { kTurnDone, kTurnFailed, kTurnInterrupted, kTurnStepCap } TurnOutcome;
typedef enum { kGateApprove, kGateRefuse } GateDecision;
// callbacks: on_text (live deltas), on_tool_start, on_tool_result,
//   gate(ud, tool, args, char **refusal_out)  <- THE chokepoint the future Lua ToolPre hook wraps,
//   on_finish(ud, outcome, err)  (exactly once)
AgentTurn *agent_turn_start(const AgentOpts *, const AgentCallbacks *, void *ud, String user_text, Error *);
void agent_turn_cancel(AgentTurn *);
```

**Per-turn state machine**: append user msg → build request (`[system] + session
messages + tools`) → stream (on_text live; provider assembles assistant msg) →
on_done: `session_append(assistant)` then, if `tool_calls` nonempty (authoritative
over the finish_reason string): for each call resolve (unknown → error result) →
parse arguments (1 MiB cap; bad JSON → error result) → **gate** (mutating+
unapproved → refuse) → execute → append `{role:"tool", tool_call_id, content}` in
order → step++ → next request; else finish. Step ≥ 50 (`MUA_STEP_CAP` env, clamp
lower-only) → finish `kTurnStepCap` (stop, never loop).

- **Trampoline driver** (no input-driven recursion): a
  `while (tool_index < tool_count)` loop with an `in_driver` flag; sync tools
  complete inline and the loop advances, async (bash) returns and re-enters via
  `tool_done_cb`. O(1) stack depth regardless of tool count.
- **Three gate implementations** next to the loop (policy, not tools.c
  mechanism): `interactive` (blocking y/N), `auto_refuse` (`-p` default),
  `approve_all` (`--yes`). The gate is a function-pointer field — exactly where
  ToolPre slots in next milestone. **Blocking fgets is sound for v1**: at gate
  time nothing user-visible is in flight (provider stream is terminal — `on_done`
  fired, retry timer closed; prior tool's handles fully closed; next tool not
  started); only curl keep-alive polling defers, harmlessly. Documented; uv_tty
  reader is the v2 path.
- **Cancellation** per phase via `loop_set_interrupt_cb` (set resets the count):
  streaming → `openrouter_cancel` → `kTurnInterrupted`; bash → `tools_cancel`
  (kill) → synthetic "interrupted" results for unresolved calls (dangling
  `tool_calls` would 400 every resumed request) → finish; in-gate fgets → EINTR
  refuses + cancels; second SIGINT → loop stops → exit 130. REPL flushes stale
  SIGINT via a new one-line `loop_run_nowait()` before arming each turn.
- **System prompt**: `#define` default + `MUA_SYSTEM_PROMPT` env override (empty =
  omit); prepended at request-build time, **not persisted** (resume must not
  double it; it's config, not dialogue). On `--resume`, repair a trailing
  unanswered `tool_calls` message with synthetic results before accepting input.

### `main.c` + REPL

- Flags: add `-y/--yes` (approve_all in any mode), `-r/--resume` (latest session;
  none → notice + fresh; works with `-p`), `-m/--model`. Usage string updated;
  exit codes unchanged (0/1/64/130).
- **REPL** (no `-p`): banner + `mua> ` on **stderr** (model text on stdout, so
  `mua | tee` captures only the model), blocking `fgets` (8 KiB line) between
  turns — loop is idle between turns (no ref'd handles, proven by today's
  `run_prompt` returning); `exit`/`quit`/EOF quits; Ctrl-C at prompt → fresh
  line; empty → reprompt. Per-tool trace chrome on stderr (`-> bash: make test` /
  `<- bash: ok`).
- **`-p`** one-shot: same machinery, single turn, `auto_refuse` unless `--yes`;
  the existing checked-`fwrite`/EPIPE-cancel `on_text` logic survives as the
  turn's text callback. `-p` turns persist (resume sees them).

## Caps table (additions)

| Cap | Value | Home |
|---|---|---|
| `MUA_SESSION_MAX_LINE` / `MAX_MESSAGES` | 8 MiB / 4096 | session.h |
| session readdir scan / id attempts / EINTR | 65536 / 100 / 100 | session.c |
| `MUA_MAX_TOOL_CALLS` | 16 | openrouter.h |
| tool args / id / name / content Buf | 256 KiB / 64 / 128 / 1 MiB | openrouter.c |
| tool_call items per delta event | 64 | openrouter.c |
| agent steps per turn | 50 (`MUA_STEP_CAP` clamp lower-only) | agent.c |
| read returned / disk scan | 256 KiB / 8 MiB | tools.c |
| edit file size | 4 MiB | tools.c |
| bash output / timeout / post-exit grace / read scratch | 64 KiB / 30 s default,120 s max / 500 ms / 8 KiB | tools.c |
| REPL line / gate line | 8 KiB / 64 B | main.c, agent.c |

## Wire reference (self-contained; OpenRouter normalizes to OpenAI)

- **Request tools**:
  `"tools":[{"type":"function","function":{"name","description","parameters":<JSON-Schema>}}]`;
  `tool_choice` omitted = auto.
- **Streamed tool_call deltas**: first fragment per call carries
  `delta.tool_calls[{index, id, type:"function", function:{name, arguments:""}}]`;
  continuations carry only `{index, function:{arguments:"<fragment>"}}`; fragments
  concatenate per index into one JSON string; parallel calls appear as `index:1`
  (possibly interleaved); terminal chunk `delta:{}, finish_reason:"tool_calls"`;
  then empty-`choices` usage chunk; then `data: [DONE]`.
- **Assembled assistant message** (appended verbatim):
  `{"role":"assistant","content":null,"tool_calls":[{"id":"call_abc","type":"function","function":{"name":"bash","arguments":"{\"command\":\"ls\"}"}}]}`
  (content = accumulated string if the model spoke first).
- **Tool result** (one per call, in order, all answered before the next assistant
  turn — OpenAI-compat 400s on dangling calls):
  `{"role":"tool","tool_call_id":"call_abc","content":"<output>"}`.
- **finish_reason**: `stop` / `tool_calls` / `length` / `content_filter`;
  provider errors arrive as the already-handled `{"error":{...}}` data chunk.

## Tests

**Unit (busted + FFI).** Prereq: move shared cJSON cdefs (`typedef struct cJSON
cJSON;`, json_parse/print/free) into `test/unit/helpers.lua` (duplicate typedefs
abort a cdef block).

- `paths_spec` additions: nested ensure-dir + mode 0700, idempotent,
  file-in-the-way (leaf and mid-path) → error.
- `session_spec` (new; `MUA_STATE_DIR` → tmpdir): header shape + file mode 0600;
  append→load round trip byte-compared via `json_print`; ownership/validation;
  collision `_00`/`_01` and latest picks `_01`; foreign-file filtering; torn-tail
  tolerance + heal byte; mid-file corruption fails naming the line;
  missing/`version:2` header errors; caps.
- `openrouter_spec` additions (via `openrouter_handle_event`, on_done re-cast):
  2-fragment arguments concat → exact assembled shape; parallel interleaved calls
  in index order; content+tool_calls; content-only → no tool_calls key; pure
  tool-call → `content:null`; index gap → skip+log; out-of-range/missing index,
  item flood, args/id/name/content overflow → terminal errors; delivered
  semantics via the new getter; existing regression specs updated to the new
  on_done arity.
- `tools_spec` (new): registry lookup/mutating flags/openai array round-trip;
  read (windows, 256 KiB truncation, NUL→binary, directory→error); edit matrix
  (0/1/2+ occurrences, empty/identical, byte-0 and EOF matches, 4 MiB cap); write
  (bytes, nested parents, unwritable→error); **bash unit-with-loop**
  (`loop_init`+FFI done-cb+`loop_run`): exit 0/3, interleave, timeout kill
  (~100 ms), >64 KiB truncation+drain, missing command → error.

**Functional (fixture server; all envs gain `MUA_STATE_DIR`).** Helper extension:
`run_mua(args, env, opts)` gains `opts.stdin` (tempfile + `< file` redirect);
local `tool_call_block(...)` SSE builder.

- `-p` persists: 3-line session file, byte-exact.
- `--resume` round trip (two server runs): request #2's `messages` == `[user,
  assistant, user]` exactly; session file grows to 5 lines; empty state dir →
  exit 1 "no sessions to resume".
- **Marquee tool round-trip** (2 blocks): block1 streams a bash `echo hi`
  tool_call (arguments split mid-fragment across `send` slices), block2 captures +
  returns text; assert request #2 has assistant-with-tool_calls (byte-faithful:
  id, `type:"function"`, name, concatenated arguments) + matching `role:"tool"`
  result, `tools` array identical in both requests, session mirrors the
  conversation.
- Refusal without `--yes` → tool result "requires approval" reaches request #2;
  read ungated in `-p`; gate approve/decline via piped stdin; REPL basics (banner
  on stderr, text on stdout, empty-line reprompt, EOF quit); `MUA_STEP_CAP=2`
  step-cap; unknown tool; two calls in one message (order); bad arguments JSON.

**Sanitized**: `make BUILD_DIR=build-san SANITIZE=1 test` — the trampoline
driver, bash close-callback ordering, and session torn-tail paths are exactly
where ASan/UBSan earn their keep.

## Commit sequence (each green; main.c rewritten once, at the end)

1. `feat(core): recursive ensure-dir in paths` — `paths_ensure_dir` + paths_spec
2. `feat(session): append-only jsonl session store` — session.{c,h}, CMake source
   entry, helpers.lua shared-cdef move (+ json_spec trim), session_spec
3. `feat(provider): tool calling with streamed tool_call accumulation` —
   openrouter v2 + internal-seam changes + start_attempt reset fix + **minimal**
   main.c migration (local one-message array, on_done frees the message) +
   openrouter_spec; stream_spec stays byte-identical. **Verify once against a live
   tool-calling response via `op run` (cheap hedge on the wire shape).**
4. `feat(tools): registry, result contract, and the read tool` + tools_spec
   (registry+read)
5. `feat(tools): write and edit with exact-match replacement`
6. `feat(tools): bash via uv_spawn with mandatory timeout and bounded capture`
7. `feat(agent): turn loop with step cap, tool gate, and cancellation` —
   agent.{c,h} + `loop_run_nowait`
8. `feat(cli): line-based repl, sessions, --resume, and one-shot agent turns` —
   main.c rewrite wiring agent+session+tools+flags; helpers.lua `stdin=` +
   `MUA_STATE_DIR` in stream/prompt envs; resume_spec + the full functional matrix
9. `docs: agent milestone state and commands` — CLAUDE.md current-state + roadmap
   tick, README usage

## Verification (per commit)

- After each commit: `make && make test && make lint`, format-stable. After 8:
  `make functionaltest` green with **no API key** (fixture-driven),
  `TEST_FILE=...stream/session/resume_spec` runs each alone.
- Sanitized full run after 6, 7, 8: `make BUILD_DIR=build-san SANITIZE=1 test`.
- Live end-to-end after 8 (via `op run` so the key never lands in
  history/transcript):
  - `OPENROUTER_API_KEY="op://Employee/OPENROUTER_API_KEY/credential" op run -- build/bin/mua -p 'read README.md and summarize it in one line'`
    → reads the file, streams a summary, exit 0; session file written.
  - REPL: `op run -- build/bin/mua`, ask it to create then edit a scratch file,
    approving each gate with `y`; then `op run -- build/bin/mua --resume` and
    confirm it recalls the prior turn.
  - `op run -- build/bin/mua -p 'run the tests'` (no `--yes`) → bash call refused,
    model adapts; rerun with `--yes` → tests run.

## Key decisions (recorded)

| Decision | Rationale |
|---|---|
| Messages = wire-shaped cJSON; session owns the array | zero-conversion requests, natural round-trip, honors json.h boundary rule; structs would need a serializer pair the moment content becomes block arrays |
| `cJSON_CreateArrayReference(messages->child)` for request body | reference array's `->child` is the passed node; verified, plus Delete skips reference children — no deep copy |
| No fsync on append | conversational state, not a WAL; torn-tail tolerance + heal byte cover OS-crash; per-message F_FULLFSYNC is costly on macOS |
| One async tool contract (sync tools complete inline) | single loop continuation path; identical to future Lua tool shape |
| bash: merged pipe, drain-past-cap, SIGKILL, 3-way close join + grace timer | true interleaving; backpressure deadlock avoided; orphaned pipe holders bounded |
| Blocking y/N gate for v1 | nothing user-visible in flight at gate time (provider terminal, no tool running); uv_tty deferred to v2 |
| System prompt not persisted | resume must not double it; it's config, not dialogue |
| `delivered` set by tool_call fragments too | one "model began answering" invariant; excludes dirty-accumulator-retry bug class |
| Session handle table deferred to API milestone | one session per process now; pure addition later (nvim find_buffer_by_handle pattern) |
| main.c rewritten once (commit 8), minimal migration in commit 3 | `-p` stays working mid-milestone without wiring sessions into a soon-deleted run_prompt |
