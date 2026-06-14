#ifndef MUA_AGENT_H
#define MUA_AGENT_H

#include <cjson/cJSON.h>

#include "mua/api/private/defs.h"
#include "mua/http.h"
#include "mua/session.h"
#include "mua/tools.h"

// The agent turn loop: append the user message, stream a response, execute
// any tool calls (sequentially, gated), append the results, repeat until the
// model stops or the step cap is hit. One AgentTurn per user input.

typedef enum {
  kTurnDone = 0,    // the model finished without further tool calls
  kTurnFailed,      // provider or session failure (err carries the detail)
  kTurnInterrupted, // canceled; unanswered tool calls got synthetic results
  kTurnStepCap,     // the hard cap stopped the turn (stop, never loop)
} TurnOutcome;

typedef enum { kGateApprove = 0, kGateRefuse } GateDecision;

// THE chokepoint the future Lua ToolPre hook wraps. Consulted for mutating
// tools only; `read` runs ungated. A refusing gate may set *refusal_out
// (xmalloc'd; the agent frees it) to explain itself to the model.
typedef GateDecision (*AgentGateFn)(void *ud, const ToolDef *tool, const cJSON *args,
                                    char **refusal_out);

typedef struct {
  void (*on_text)(void *ud, const String *text); // live deltas, pass-through
  void (*on_tool_start)(void *ud, const char *name, const cJSON *args);
  void (*on_tool_result)(void *ud, const char *name, const ToolResult *result); // borrowed
  AgentGateFn gate;                                                   // NULL behaves as approve_all
  void (*on_finish)(void *ud, TurnOutcome outcome, const Error *err); // exactly once
} AgentCallbacks;

typedef struct {
  HttpClient *http;      // borrowed; the caller owns its lifecycle
  SessionState *session; // borrowed; the conversation of record
  const char *model;     // NULL -> provider default
  const char *api_key;   // required (the provider validates)
} AgentOpts;

typedef struct AgentTurn AgentTurn;

// Starts one turn. Returns NULL + err on synchronous failure (no callback
// fires); otherwise on_finish fires exactly once and the handle is invalid
// the moment it does (callers null their reference inside on_finish).
// Arms the loop's SIGINT callback for the turn; cleared again on finish.
AgentTurn *agent_turn_start(const AgentOpts *opts, const AgentCallbacks *cb, void *ud,
                            String user_text, Error *err);

// Phase-aware cancellation: a streaming response is canceled at the
// provider; a running tool is killed; unanswered tool calls get synthetic
// "[interrupted]" results so the session never records a dangling call.
// on_finish still fires (kTurnInterrupted).
void agent_turn_cancel(AgentTurn *turn);

// Heals a resumed session whose tail is an assistant message with unanswered
// tool_calls -- the crash/poison artifact a live cancel would have filled.
// Appends a synthetic role:"tool" "[interrupted]" result for every call id
// not already answered, so the next request is well-formed (a dangling call
// makes every resumed request 400). A no-op when nothing dangles. Call once
// after a resume load, before the first turn. false + err only on an append
// failure.
bool agent_repair_session(SessionState *sess, Error *err);

// The three gate policies, next to the loop (policy, not tools.c mechanism).
GateDecision agent_gate_interactive(void *ud, const ToolDef *tool, const cJSON *args,
                                    char **refusal_out); // blocking y/N on stderr/stdin
GateDecision agent_gate_auto_refuse(void *ud, const ToolDef *tool, const cJSON *args,
                                    char **refusal_out); // -p default
GateDecision agent_gate_approve_all(void *ud, const ToolDef *tool, const cJSON *args,
                                    char **refusal_out); // --yes

#endif // MUA_AGENT_H
