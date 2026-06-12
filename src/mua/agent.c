#include "mua/agent.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "mua/api/private/helpers.h"
#include "mua/json.h"
#include "mua/log.h"
#include "mua/loop.h"
#include "mua/memory.h"
#include "mua/provider/openrouter.h"

enum {
  kAgentDefaultStepCap = 50,   // MUA_STEP_CAP clamps lower only
  kAgentArgsCap = 1024 * 1024, // parse bound for one call's arguments string
  kAgentGateLine = 64,
  kAgentGateDrainMax = 8192, // overlong gate input drained up to this many bytes
};

#define MUA_DEFAULT_SYSTEM_PROMPT                                                                  \
  "You are mua, a minimal coding agent. Use the provided tools to read, "                          \
  "write, and edit files and to run shell commands in the current working "                        \
  "directory. Keep responses brief."

struct AgentTurn {
  AgentOpts opts; // borrowed pointers within; the caller outlives the turn
  AgentCallbacks cb;
  void *ud;
  char *system_prompt; // resolved once at start; NULL means omit
  int step;            // completed tool rounds
  int step_cap;
  OpenrouterStream *stream; // live while streaming, else NULL
  ToolExec *exec;           // live while an async tool runs, else NULL
  const cJSON *assistant;   // borrowed from the session: this step's message
  const cJSON *calls;       // borrowed tool_calls array of `assistant`
  int tool_index;
  int tool_count;
  bool in_driver;        // drive_tools is on the stack; done-cbs must not re-enter
  bool cancel_requested; // canceled while streaming
  bool canceling;        // canceled during tools; drain, then synthetics
  bool failing;          // a session append failed; finish kTurnFailed
  Error fail_err;
  bool finished;
};

static char *aformat(const char *fmt, ...) MUA_PRINTF_ATTR(1, 2);

static char *aformat(const char *fmt, ...)
{
  va_list ap;
  va_start(ap, fmt);
  va_list ap2;
  va_copy(ap2, ap);
  int need = vsnprintf(NULL, 0, fmt, ap);
  va_end(ap);
  if (need < 0) {
    need = 0; // a formatting failure degrades to an empty message, not UB
  }
  char *msg = xmalloc((size_t)need + 1);
  msg[0] = '\0';
  (void)vsnprintf(msg, (size_t)need + 1, fmt, ap2); // sized by the first pass
  va_end(ap2);
  return msg;
}

// MUA_SYSTEM_PROMPT overrides the built-in default; empty omits it entirely.
// Config, not dialogue: never persisted, injected at request build only.
static char *resolve_system_prompt(void)
{
  const char *env = getenv("MUA_SYSTEM_PROMPT");
  if (env != NULL) {
    return env[0] != '\0' ? xstrdup(env) : NULL;
  }
  return xstrdup(MUA_DEFAULT_SYSTEM_PROMPT);
}

// MUA_STEP_CAP clamps lower only: a hostile environment cannot raise the
// bound, and nonsense values fall back to the default.
static int resolve_step_cap(void)
{
  const char *env = getenv("MUA_STEP_CAP");
  if (env == NULL || env[0] == '\0') {
    return kAgentDefaultStepCap;
  }
  char *end = NULL;
  long value = strtol(env, &end, 10);
  if (end == env || *end != '\0' || value < 1 || value > kAgentDefaultStepCap) {
    return kAgentDefaultStepCap;
  }
  return (int)value;
}

// The single exit: latches, disarms SIGINT, fires on_finish exactly once,
// clears `err` (which may point into the turn), and frees the turn.
static void finish_turn(AgentTurn *turn, TurnOutcome outcome, Error *err)
{
  if (turn->finished) {
    api_clear_error(err);
    return;
  }
  turn->finished = true;
  loop_set_interrupt_cb(NULL, NULL); // a later SIGINT must not touch a freed turn
  turn->cb.on_finish(turn->ud, outcome, err);
  api_clear_error(err);
  xfree(turn->system_prompt);
  xfree(turn);
}

static void drive_tools(AgentTurn *turn);

static const char *call_name(const AgentTurn *turn, int idx)
{
  const cJSON *call = cJSON_GetArrayItem(turn->calls, idx);
  const cJSON *function = (call != NULL) ? json_get_obj(call, "function") : NULL;
  const char *name = (function != NULL) ? json_get_cstr(function, "name") : NULL;
  return name != NULL ? name : "(unnamed)";
}

// Builds and appends the role:"tool" answer for call `idx`.
static bool append_tool_message(AgentTurn *turn, int idx, const char *content, Error *err)
{
  const cJSON *call = cJSON_GetArrayItem(turn->calls, idx);
  const char *id = (call != NULL) ? json_get_cstr(call, "id") : NULL;
  cJSON *msg = json_new_obj();
  json_add_cstr(msg, "role", "tool");
  json_add_cstr(msg, "tool_call_id", id != NULL ? id : "");
  json_add_cstr(msg, "content", content != NULL ? content : "");
  return session_append(turn->opts.session, msg, err); // owns msg, even on failure
}

// Pure state advance for one completed call (synthetic or real): append the
// role:"tool" answer, report it, step the index. Never re-enters the driver —
// that belongs to agent_tool_done alone, which keeps the direct call graph
// acyclic (the runtime cycle is broken by in_driver; the static one by this
// split).
static void tool_completed(AgentTurn *turn, const ToolResult *result)
{
  int idx = turn->tool_index;
  Error err = ERROR_INIT;
  if (!append_tool_message(turn, idx, result->content, &err)) {
    xfree(result->content);
    turn->failing = true; // contract 2: the message is gone; stop appending
    turn->fail_err = err; // ownership of err.msg moves into the turn
    return;
  }
  if (turn->cb.on_tool_result != NULL) {
    turn->cb.on_tool_result(turn->ud, call_name(turn, idx), result);
  }
  xfree(result->content); // ownership transferred to us by the done contract
  turn->tool_index++;
}

// The ToolDoneCb handed to execute(): tools invoke it through the function
// pointer, inline (sync) or from the event loop (async). Only the async case
// re-enters the driver; inline completions are picked up by the loop already
// on the stack.
static void agent_tool_done(void *ud, const ToolResult *result)
{
  AgentTurn *turn = ud;
  turn->exec = NULL; // the handle is invalid once done fires
  if (turn->finished) {
    xfree(result->content); // late async completion after the turn ended
    return;
  }
  tool_completed(turn, result);
  if (!turn->in_driver) {
    drive_tools(turn);
  }
}

// Fires a synthetic is_error result through the normal completion path.
// Always called under the driver, so no re-entry is needed. Takes ownership
// of `content`. Returns NULL exactly like a sync tool.
static ToolExec *synthetic_result(AgentTurn *turn, char *content)
{
  ToolResult result = {.content = content, .is_error = true};
  tool_completed(turn, &result);
  return NULL;
}

// Starts call `idx`: resolve, parse arguments, gate, execute. Bad calls are
// results the model sees (never turn failures); only infrastructure fails.
static ToolExec *start_tool(AgentTurn *turn, int idx)
{
  const cJSON *call = cJSON_GetArrayItem(turn->calls, idx);
  const cJSON *function = (call != NULL) ? json_get_obj(call, "function") : NULL;
  const char *name = (function != NULL) ? json_get_cstr(function, "name") : NULL;
  const char *args_text = (function != NULL) ? json_get_cstr(function, "arguments") : NULL;
  const ToolDef *def = tools_lookup(name);
  if (def == NULL) {
    return synthetic_result(turn, aformat("unknown tool: %s", name != NULL ? name : "(unnamed)"));
  }
  if (args_text == NULL || args_text[0] == '\0') {
    args_text = "{}"; // a zero-fragment arguments string means "no arguments"
  }
  Error perr = ERROR_INIT;
  cJSON *args = json_parse(cstr_as_string(args_text), kAgentArgsCap, &perr);
  api_clear_error(&perr); // the failure detail is not for the model
  if (args == NULL) {
    return synthetic_result(turn, aformat("%s: arguments are not valid JSON", def->name));
  }
  if (def->mutating && turn->cb.gate != NULL) {
    char *refusal = NULL;
    if (turn->cb.gate(turn->ud, def, args, &refusal) == kGateRefuse) {
      json_free(args);
      char *content = (refusal != NULL) ? refusal : xstrdup("refused by the tool gate");
      return synthetic_result(turn, content);
    }
    xfree(refusal); // an approving gate should not set it, but never leak
  }
  if (turn->cb.on_tool_start != NULL) {
    turn->cb.on_tool_start(turn->ud, def->name, args);
  }
  ToolExec *exec = def->execute(args, agent_tool_done, turn);
  json_free(args); // the borrow ends when execute returns
  return exec;
}

// Contract 5: a dangling tool_calls message must always be answered, or
// every resumed request 400s. Answer whatever the cancellation left open.
static void finish_with_synthetics(AgentTurn *turn)
{
  Error err = ERROR_INIT;
  while (turn->tool_index < turn->tool_count) { // bounded: <= MUA_MAX_TOOL_CALLS
    if (!append_tool_message(turn, turn->tool_index, "[interrupted]", &err)) {
      log_msg(kLogWarn, "agent: interrupted-result append failed: %s",
              err.msg != NULL ? err.msg : "(no detail)");
      api_clear_error(&err);
      break; // poisoned session: the --resume repair owns the file from here
    }
    turn->tool_index++;
  }
  Error none = ERROR_INIT;
  finish_turn(turn, kTurnInterrupted, &none);
}

static void next_step(AgentTurn *turn);

// The trampoline: sync tools complete inline and the loop advances; an async
// tool returns its handle and the matching done callback re-enters. O(1)
// stack depth regardless of tool count; no input-driven recursion.
static void drive_tools(AgentTurn *turn)
{
  turn->in_driver = true;
  while (turn->tool_index < turn->tool_count && !turn->canceling && !turn->failing) {
    ToolExec *exec = start_tool(turn, turn->tool_index);
    if (exec != NULL) {
      turn->exec = exec;
      turn->in_driver = false;
      return; // agent_tool_done re-enters when the tool finishes
    }
  }
  turn->in_driver = false;
  if (turn->failing) {
    finish_turn(turn, kTurnFailed, &turn->fail_err);
    return;
  }
  if (turn->canceling) {
    finish_with_synthetics(turn);
    return;
  }
  next_step(turn);
}

static void provider_on_text(void *ud, const String *text)
{
  AgentTurn *turn = ud;
  if (turn->cb.on_text != NULL) {
    turn->cb.on_text(turn->ud, text);
  }
}

static void provider_on_error(void *ud, const Error *perr)
{
  AgentTurn *turn = ud;
  turn->stream = NULL;
  if (turn->cancel_requested) {
    Error none = ERROR_INIT;
    finish_turn(turn, kTurnInterrupted, &none);
    return;
  }
  Error err = ERROR_INIT;
  api_set_error(&err, perr->type, "%s", perr->msg != NULL ? perr->msg : "provider failure");
  finish_turn(turn, kTurnFailed, &err);
}

static void provider_on_done(void *ud, cJSON *message, const String *finish_reason,
                             int64_t completion_tokens)
{
  AgentTurn *turn = ud;
  turn->stream = NULL;
  (void)finish_reason; // a nonempty tool_calls array is authoritative (contract 6)
  (void)completion_tokens;
  Error err = ERROR_INIT;
  if (!session_append(turn->opts.session, message, &err)) { // owns even on failure
    finish_turn(turn, kTurnFailed, &err);
    return;
  }
  size_t count = session_message_count(turn->opts.session);
  turn->assistant = session_message_get(turn->opts.session, count - 1);
  const cJSON *calls = json_get_arr(turn->assistant, "tool_calls");
  int call_count = (calls != NULL) ? cJSON_GetArraySize(calls) : 0;
  if (call_count <= 0) {
    Error none = ERROR_INIT;
    finish_turn(turn, kTurnDone, &none); // the model completed; Done even if a
    return;                              // cancel raced the final chunk
  }
  turn->calls = calls;
  turn->tool_count = call_count;
  turn->tool_index = 0;
  if (turn->cancel_requested) {
    turn->canceling = true;       // canceled as the response landed: answer the
    finish_with_synthetics(turn); // calls synthetically, run nothing
    return;
  }
  drive_tools(turn);
}

// Builds the request and starts the stream. Messages are the session array
// itself (pure zero-copy, contract 1) unless a system prompt is set, in
// which case a temp array holds one owned system message plus reference
// shells — the payload trees stay borrowed from the session either way.
static bool issue_request(AgentTurn *turn, Error *err)
{
  cJSON *tools = tools_build_openai_array(err);
  if (tools == NULL) {
    return false;
  }
  cJSON *request_messages = NULL;
  const cJSON *messages = session_messages(turn->opts.session);
  if (turn->system_prompt != NULL) {
    request_messages = cJSON_CreateArray();
    cJSON *system = json_new_obj();
    json_add_cstr(system, "role", "system");
    json_add_cstr(system, "content", turn->system_prompt);
    cJSON_AddItemToArray(request_messages, system);
    for (const cJSON *msg = messages->child; msg != NULL; msg = msg->next) { // <= 4096
      cJSON_AddItemToArray(request_messages, cJSON_CreateObjectReference(msg));
    }
    messages = request_messages;
  }
  OpenrouterOpts opts = {
    .model = turn->opts.model,
    .messages = messages,
    .tools = tools,
    .api_key = turn->opts.api_key,
  };
  OpenrouterCallbacks cbs = {
    .on_text = provider_on_text,
    .on_done = provider_on_done,
    .on_error = provider_on_error,
  };
  turn->stream = openrouter_stream(turn->opts.http, &opts, &cbs, turn, err);
  json_free(tools);            // borrowed only for the call (contract 3)
  json_free(request_messages); // NULL-safe; frees the shells, never the session trees
  return turn->stream != NULL;
}

static void next_step(AgentTurn *turn)
{
  turn->step++;
  if (turn->step >= turn->step_cap) {
    Error none = ERROR_INIT;
    finish_turn(turn, kTurnStepCap, &none); // stop, never loop
    return;
  }
  Error err = ERROR_INIT;
  if (!issue_request(turn, &err)) {
    finish_turn(turn, kTurnFailed, &err);
  }
}

static void agent_interrupt_cb(void *ud)
{
  agent_turn_cancel(ud);
}

void agent_turn_cancel(AgentTurn *turn)
{
  if (turn == NULL || turn->finished) {
    return;
  }
  if (turn->stream != NULL) {
    turn->cancel_requested = true;
    openrouter_cancel(turn->stream); // its on_error finishes the turn
    return;
  }
  turn->canceling = true;
  if (turn->exec != NULL) {
    tools_cancel(turn->exec); // its done answers this call, then the driver
    return;                   // sees `canceling` and writes the synthetics
  }
  // Inside the driver or the gate: the canceling flag stops the loop at the
  // next iteration boundary, which then writes the synthetics.
}

AgentTurn *agent_turn_start(const AgentOpts *opts, const AgentCallbacks *cb, void *ud,
                            String user_text, Error *err)
{
  if (opts == NULL || opts->http == NULL || opts->session == NULL || cb == NULL ||
      cb->on_finish == NULL) {
    api_set_error(err, kErrorTypeValidation, "agent: http, session, and on_finish are required");
    return NULL;
  }
  if (user_text.data == NULL || user_text.size == 0) {
    api_set_error(err, kErrorTypeValidation, "agent: user text is empty");
    return NULL;
  }
  cJSON *user = json_new_obj();
  json_add_cstr(user, "role", "user");
  json_add_str(user, "content", user_text);
  if (!session_append(opts->session, user, err)) {
    return NULL; // append owned and freed it; the file records nothing
  }
  AgentTurn *turn = xcalloc(1, sizeof *turn);
  turn->opts = *opts;
  turn->cb = *cb;
  turn->ud = ud;
  turn->system_prompt = resolve_system_prompt();
  turn->step_cap = resolve_step_cap();
  if (!issue_request(turn, err)) {
    xfree(turn->system_prompt);
    xfree(turn);
    return NULL; // the user message stays in the session: an honest record
  }
  loop_set_interrupt_cb(agent_interrupt_cb, turn);
  return turn;
}

GateDecision agent_gate_approve_all(void *ud, const ToolDef *tool, const cJSON *args,
                                    char **refusal_out)
{
  (void)ud;
  (void)tool;
  (void)args;
  (void)refusal_out;
  return kGateApprove;
}

GateDecision agent_gate_auto_refuse(void *ud, const ToolDef *tool, const cJSON *args,
                                    char **refusal_out)
{
  (void)ud;
  (void)args;
  *refusal_out = aformat("%s requires approval; re-run with --yes", tool->name);
  return kGateRefuse;
}

// Drains an overlong gate line so leftovers cannot poison the next read.
static void gate_drain_line(void)
{
  for (int i = 0; i < kAgentGateDrainMax; i++) {
    int c = getchar();
    if (c == '\n' || c == EOF) {
      return;
    }
  }
}

GateDecision agent_gate_interactive(void *ud, const ToolDef *tool, const cJSON *args,
                                    char **refusal_out)
{
  (void)ud;
  (void)args;
  // Blocking is sound here: nothing user-visible is in flight at gate time
  // (the provider stream is terminal, the prior tool fully closed, the next
  // tool unstarted); only curl keep-alive polling defers, harmlessly.
  (void)fprintf(stderr, "mua: allow %s? [y/N] ", tool->name);
  (void)fflush(stderr);
  char line[kAgentGateLine];
  if (fgets(line, sizeof line, stdin) == NULL) {
    // EOF, or EINTR from ctrl-C: refuse; the queued SIGINT cancels the turn
    // at the next loop entry.
    clearerr(stdin);
    *refusal_out = aformat("%s declined at the approval prompt", tool->name);
    return kGateRefuse;
  }
  if (strchr(line, '\n') == NULL) {
    gate_drain_line();
  }
  if (line[0] == 'y' || line[0] == 'Y') {
    return kGateApprove;
  }
  *refusal_out = aformat("%s declined at the approval prompt", tool->name);
  return kGateRefuse;
}
