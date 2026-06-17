#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <cjson/cJSON.h>

#include "auto/versiondef.h"
#include "mua/agent.h"
#include "mua/api/private/helpers.h"
#include "mua/autocmd.h"
#include "mua/http.h"
#include "mua/json.h"
#include "mua/log.h"
#include "mua/loop.h"
#include "mua/lua/autocmd.h"
#include "mua/lua/state.h"
#include "mua/memory.h"
#include "mua/options.h"
#include "mua/provider/models.h"
#include "mua/render.h"
#include "mua/rpc.h"
#include "mua/session.h"
#include "mua/tools.h"

enum {
  kExitOk = 0,
  kExitFailure = 1,
  kExitUsage = 64,
  kExitInterrupted = 130,
};

enum {
  kReplLine = 8192,
  kReplDrainMax = 1 << 20,
  kToolArgPreview = 120,
  kOutcomeUnset = -1,      // distinguishes a second-SIGINT loop stop from a real finish
  kRenderOutMax = 4 << 20, // styled-output buffer; drained+reset each feed, never accumulates
};

typedef struct {
  bool version;
  bool help;
  bool yes;
  bool resume;
  bool embed;
  const char *prompt;
  const char *model;
} MuaArgs;

typedef struct {
  AgentTurn *turn; // set before loop_run; nulled in on_finish
  int outcome;     // kOutcomeUnset until on_finish fires
  bool wrote_any;
  bool last_was_newline;
  bool failed_write;     // a stdout write failed (EPIPE under `| head`)
  AgentGateFn base_gate; // the chosen policy; gate_with_autocmds wraps it
  bool render_on;        // markdown rendering: isatty(stdout) && mua.o.markdown, set per turn
  Renderer rndr;         // markdown renderer state (used only when render_on)
  Buf out;               // styled bytes drained to stdout via drain_out (only when render_on)
} TurnCtx;

static void print_usage(FILE *stream)
{
  // Diagnostic path; nothing to do if the stream is gone.
  (void)fprintf(stream, "Usage: mua [--version] [--help] [-p TEXT] [-y|--yes] "
                        "[-r|--resume] [-m|--model MODEL] [--embed]\n");
}

static bool parse_args(int argc, char **argv, MuaArgs *out)
{
  *out = (MuaArgs){0};
  for (int i = 1; i < argc; i++) {
    const char *arg = argv[i];
    const char *value = (i + 1 < argc) ? argv[i + 1] : NULL;
    if (strcmp(arg, "--version") == 0) {
      out->version = true;
    } else if (strcmp(arg, "-h") == 0 || strcmp(arg, "--help") == 0) {
      out->help = true;
    } else if (strcmp(arg, "-y") == 0 || strcmp(arg, "--yes") == 0) {
      out->yes = true;
    } else if (strcmp(arg, "-r") == 0 || strcmp(arg, "--resume") == 0) {
      out->resume = true;
    } else if (strcmp(arg, "--embed") == 0) {
      out->embed = true;
    } else if (strcmp(arg, "-p") == 0 || strcmp(arg, "--prompt") == 0) {
      if (value == NULL) {
        (void)fprintf(stderr, "mua: missing argument for %s\n", arg);
        return false;
      }
      out->prompt = value;
      i++;
    } else if (strcmp(arg, "-m") == 0 || strcmp(arg, "--model") == 0) {
      if (value == NULL) {
        (void)fprintf(stderr, "mua: missing argument for %s\n", arg);
        return false;
      }
      out->model = value;
      i++;
    } else {
      (void)fprintf(stderr, "mua: unknown argument: %s\n", arg);
      return false;
    }
  }
  return true;
}

// Writes the renderer's styled bytes to stdout and resets the buffer. The
// single styled-write site, so EPIPE handling stays in one place. Returns false
// on a write failure (stdout gone under `| head`).
static bool drain_out(TurnCtx *ctx)
{
  if (ctx->out.size == 0) {
    return true;
  }
  if (fwrite(ctx->out.data, 1, ctx->out.size, stdout) != ctx->out.size || fflush(stdout) != 0) {
    return false;
  }
  ctx->wrote_any = true;
  buf_reset(&ctx->out);
  return true;
}

static void on_text(void *ud, const String *text)
{
  TurnCtx *ctx = ud;
  if (ctx->failed_write || text->size == 0) {
    return;
  }
  if (ctx->render_on) {
    // Styled path: feed appends to ctx->out, drain_out writes it. A false from
    // either is a write failure, handled exactly like the raw path below.
    if (!render_feed(&ctx->rndr, *text, &ctx->out) || !drain_out(ctx)) {
      ctx->failed_write = true;
      agent_turn_cancel(ctx->turn);
      return;
    }
  } else {
    if (fwrite(text->data, 1, text->size, stdout) != text->size || fflush(stdout) != 0) {
      ctx->failed_write = true; // stdout is gone; stop the turn
      agent_turn_cancel(ctx->turn);
      return;
    }
    ctx->wrote_any = true;
    ctx->last_was_newline = (text->data[text->size - 1] == '\n');
  }
  mua_lua_autocmd_stream_delta(*text); // StreamDelta hooks see RAW text (no-op without one)
}

static void on_tool_start(void *ud, const char *name, const cJSON *args)
{
  (void)ud;
  String preview = json_print(args);
  int shown = (int)(preview.size < kToolArgPreview ? preview.size : kToolArgPreview);
  (void)fprintf(stderr, "mua: -> %s %.*s%s\n", name, shown,
                preview.data != NULL ? preview.data : "",
                preview.size > kToolArgPreview ? "..." : "");
  xfree(preview.data);
}

static void on_tool_result(void *ud, const char *name, const ToolResult *result)
{
  (void)ud;
  (void)fprintf(stderr, "mua: <- %s: %s\n", name, result->is_error ? "error" : "ok");
  mua_lua_autocmd_tool_post(name, result->is_error, cstr_as_string(result->content));
}

// Out-of-band agent notices (e.g. the context-window warning) go to stderr,
// alongside the other chrome, leaving stdout for the model's output.
static void on_notice(void *ud, const String *msg)
{
  (void)ud;
  (void)fprintf(stderr, "mua: %.*s\n", (int)msg->size, msg->data != NULL ? msg->data : "");
}

static void on_finish(void *ud, TurnOutcome outcome, const Error *err)
{
  TurnCtx *ctx = ud;
  ctx->outcome = (int)outcome;
  // Flush any line the renderer was still buffering before deciding the
  // trailing newline (the renderer owns whether output ended on a '\n', since
  // it may have stripped a marker or added an SGR off-code as the last byte).
  if (ctx->render_on && !ctx->failed_write) {
    if (!render_flush(&ctx->rndr, &ctx->out) || !drain_out(ctx)) {
      ctx->failed_write = true;
    }
  }
  bool ended_newline = ctx->render_on ? ctx->rndr.ended_newline : ctx->last_was_newline;
  // Cosmetic trailing newline on success only; an error/interrupt leaves the
  // partial output as-is and explains itself on stderr.
  if (outcome == kTurnDone && ctx->wrote_any && !ended_newline && !ctx->failed_write) {
    (void)fputc('\n', stdout);
    (void)fflush(stdout);
  }
  if (!ctx->failed_write) {
    if (outcome == kTurnFailed) {
      (void)fprintf(stderr, "mua: %s\n", (err != NULL && err->msg != NULL) ? err->msg : "failed");
    } else if (outcome == kTurnStepCap) {
      (void)fprintf(stderr, "mua: step cap reached\n");
    } else if (outcome == kTurnContextLimit) {
      (void)fprintf(stderr, "mua: context window budget reached\n");
    } else if (outcome == kTurnInterrupted) {
      (void)fprintf(stderr, "mua: interrupted\n");
    }
  }
  ctx->turn = NULL;
}

// Resolves the session: a fresh one, or the resumed latest (repaired). A
// missing latest is a notice + fresh start; a corrupt one fails hard so the
// data loss is never silently hidden. NULL + err on a hard failure.
static SessionState *setup_session(bool resume, Error *err)
{
  if (!resume) {
    return session_new(err);
  }
  SessionState *sess = session_load_latest(err);
  if (sess == NULL) {
    if (err->msg != NULL && strstr(err->msg, "no sessions to resume") != NULL) {
      api_clear_error(err);
      (void)fprintf(stderr, "mua: no session to resume; starting a new one\n");
      return session_new(err);
    }
    return NULL; // corrupt latest (or worse): the caller reports err
  }
  if (!agent_repair_session(sess, err)) {
    session_free(sess);
    return NULL;
  }
  return sess;
}

// Composing gate: a Lua ToolPre hook may veto any tool; otherwise the chosen
// policy (ctx->base_gate) decides. ToolPre thus fires for every tool, while a
// human y/N still applies only to mutating ones. A veto holds even under --yes
// -- it is a programmatic policy, distinct from human approval.
static GateDecision gate_with_autocmds(void *ud, const ToolDef *tool, const cJSON *args,
                                       cJSON **rewrite_out, char **refusal_out)
{
  TurnCtx *ctx = ud;
  const cJSON *effective = args; // the rewrite, if a hook returns one; else as-proposed
  if (autocmd_count(kAutocmdToolPre) > 0) {
    Object args_obj = NIL;
    Error err = ERROR_INIT;
    if (cjson_to_object(args, &args_obj, &err)) {
      char *reason = NULL;
      Object rewrite = NIL;
      bool vetoed = mua_lua_autocmd_tool_pre(tool->name, args_obj, &rewrite, &reason);
      api_free_object(&args_obj);
      if (vetoed) {
        api_free_object(&rewrite); // a veto wins; drop any pending rewrite (NIL-safe)
        *refusal_out = (reason != NULL) ? reason : xstrdup("vetoed by a ToolPre hook");
        return kGateRefuse;
      }
      if (rewrite.type != kObjectTypeNil) {
        cJSON *new_args = object_to_cjson(&rewrite);
        api_free_object(&rewrite);
        if (new_args != NULL && cJSON_IsObject(new_args)) {
          *rewrite_out = new_args; // the agent takes ownership and runs these
          effective = new_args;    // the base gate (and its y/N) sees the rewrite
        } else {
          if (new_args != NULL) {
            json_free(new_args); // tool args must be a JSON object; ignore otherwise
          }
          log_msg(kLogWarn, "autocmd: ToolPre rewrite ignored (not a JSON object)");
        }
      }
    } else {
      log_msg(kLogWarn, "autocmd: ToolPre args marshal failed: %s",
              err.msg != NULL ? err.msg : "?");
      api_clear_error(&err);
    }
  }
  return ctx->base_gate(ud, tool, effective, rewrite_out, refusal_out);
}

// Runs one turn to completion. Returns the exit code; *hard_stop is set when a
// second SIGINT stopped the loop with the turn unfinished (on_finish never
// fired) so the REPL can quit rather than reprompt.
static int run_one_turn(HttpClient *http, SessionState *sess, AgentGateFn gate, const char *model,
                        const char *api_key, int64_t context_length, const char *text,
                        bool *hard_stop)
{
  *hard_stop = false;
  TurnCtx ctx = {.turn = NULL, .outcome = kOutcomeUnset, .base_gate = gate};
  // Markdown rendering: opt-in (mua.o.markdown) and only on a real terminal, so
  // piped output stays byte-identical. Decided once; the store is stable mid-run.
  ctx.render_on = isatty(STDOUT_FILENO) && options_markdown();
  if (ctx.render_on) {
    render_init(&ctx.rndr);
    buf_init(&ctx.out, kRenderOutMax);
  }
  AgentOpts opts = {.http = http,
                    .session = sess,
                    .model = model,
                    .api_key = api_key,
                    .context_length = context_length};
  AgentCallbacks cbs = {.on_text = on_text,
                        .on_tool_start = on_tool_start,
                        .on_tool_result = on_tool_result,
                        .on_notice = on_notice,
                        .gate = gate_with_autocmds,
                        .on_finish = on_finish};
  loop_run_nowait(); // drain a SIGINT pressed at the prompt before arming the turn
  Error err = ERROR_INIT;
  ctx.turn = agent_turn_start(&opts, &cbs, &ctx, cstr_as_string(text), &err);
  if (ctx.turn == NULL) {
    (void)fprintf(stderr, "mua: %s\n", err.msg != NULL ? err.msg : "cannot start turn");
    api_clear_error(&err);
    return kExitFailure;
  }
  (void)loop_run();
  if (ctx.render_on) {
    // on_finish (inside loop_run) already flushed on a normal finish; on a
    // second-SIGINT hard stop it never fired and the pending line is discarded,
    // matching the "leave partial output as-is" contract on interrupt.
    render_free(&ctx.rndr);
    buf_free(&ctx.out);
  }
  if (ctx.outcome == kOutcomeUnset) {
    *hard_stop = true; // second SIGINT: the loop stopped, the turn is unfinished
    return kExitInterrupted;
  }
  if (ctx.failed_write) {
    return kExitFailure; // EPIPE outranks the cancel it triggered
  }
  if (ctx.outcome == kTurnDone) {
    return kExitOk;
  }
  return ctx.outcome == kTurnInterrupted ? kExitInterrupted : kExitFailure;
}

static void drain_line(void)
{
  for (int i = 0; i < kReplDrainMax; i++) { // bounded
    int c = getchar();
    if (c == '\n' || c == EOF) {
      return;
    }
  }
}

static int run_repl(HttpClient *http, SessionState *sess, const char *model, const char *api_key,
                    int64_t context_length, bool yes)
{
  AgentGateFn gate = yes ? agent_gate_approve_all : agent_gate_interactive;
  (void)fprintf(stderr, "mua %s -- type 'exit' or Ctrl-D to quit\n", MUA_VERSION_STRING);
  char line[kReplLine];
  for (;;) {
    (void)fprintf(stderr, "mua> ");
    (void)fflush(stderr);
    if (fgets(line, sizeof line, stdin) == NULL) {
      (void)fputc('\n', stderr);
      if (feof(stdin)) {
        return kExitOk; // Ctrl-D
      }
      clearerr(stdin); // Ctrl-C at the prompt: fresh line
      continue;
    }
    size_t len = strlen(line);
    if (len > 0 && line[len - 1] == '\n') {
      line[len - 1] = '\0';
    } else if (len == sizeof line - 1) {
      drain_line();
      (void)fprintf(stderr, "mua: input too long; ignored\n");
      continue;
    }
    if (line[0] == '\0') {
      continue; // empty line reprompts
    }
    if (strcmp(line, "exit") == 0 || strcmp(line, "quit") == 0) {
      return kExitOk;
    }
    bool hard_stop = false;
    int code = run_one_turn(http, sess, gate, model, api_key, context_length, line, &hard_stop);
    if (hard_stop) {
      return code; // second SIGINT exits the REPL
    }
  }
}

static int run_agent(const MuaArgs *args)
{
  const char *api_key = getenv("OPENROUTER_API_KEY");
  if (api_key == NULL || api_key[0] == '\0') {
    (void)fprintf(stderr, "mua: OPENROUTER_API_KEY is not set\n");
    return kExitFailure;
  }
  Error err = ERROR_INIT;
  HttpClient *http = http_client_new(loop_get(), &err);
  if (http == NULL) {
    (void)fprintf(stderr, "mua: %s\n", err.msg != NULL ? err.msg : "http init failed");
    api_clear_error(&err);
    return kExitFailure;
  }
  int code = kExitFailure;
  SessionState *sess = setup_session(args->resume, &err);
  if (sess == NULL) {
    (void)fprintf(stderr, "mua: %s\n", err.msg != NULL ? err.msg : "cannot open session");
    api_clear_error(&err);
  } else {
    // Effective model: CLI -m wins, else mua.o.model from init.lua, else NULL
    // (the provider default). The store outlives the turn, so borrowing is safe.
    const char *model = args->model != NULL ? args->model : options_model_borrow();
    // The context-window budget needs the model's window. MUA_CONTEXT_LENGTH is
    // a per-invocation override (and the test seam): when set it wins (0 there
    // disables the budget); otherwise fetch it from the catalog once. A failed
    // fetch returns 0 -> budget disabled, the step cap still governs.
    int64_t context_length = 0;
    const char *env_ctx = getenv("MUA_CONTEXT_LENGTH");
    if (env_ctx != NULL && env_ctx[0] != '\0') {
      char *end = NULL;
      long value = strtol(env_ctx, &end, 10);
      if (end != env_ctx && *end == '\0' && value >= 0) {
        context_length = (int64_t)value;
      }
    } else {
      context_length = models_fetch_context_length(http, model, api_key, NULL);
    }
    session_set_current(sess); // register the run's session so hooks resolve `0`
    mua_lua_autocmd_session(kAutocmdSessionStart, session_id(sess));
    if (args->prompt != NULL) {
      AgentGateFn gate = args->yes ? agent_gate_approve_all : agent_gate_auto_refuse;
      bool hard_stop = false;
      code =
        run_one_turn(http, sess, gate, model, api_key, context_length, args->prompt, &hard_stop);
    } else {
      code = run_repl(http, sess, model, api_key, context_length, args->yes);
    }
    mua_lua_autocmd_session(kAutocmdSessionEnd, session_id(sess));
    session_set_current(NULL); // clear the borrow before the SessionState is freed
  }
  session_free(sess); // NULL-safe; the turn borrowed it and is finished
  http_client_close(http);
  (void)loop_run(); // drain the client's deferred handle closes
  return code;
}

static int run(const MuaArgs *args)
{
  if (!loop_init()) {
    (void)fprintf(stderr, "mua: failed to initialize event loop\n");
    return kExitFailure;
  }
  json_init();
  int code = kExitOk;
  if (!mua_lua_init()) {
    (void)fprintf(stderr, "mua: failed to initialize lua\n");
    code = kExitFailure;
  } else {
    // A broken init.lua is nonfatal by contract; sourcing fails only on
    // internal errors.
    if (!mua_lua_source_init()) {
      (void)fprintf(stderr, "mua: failed to source init.lua\n");
      code = kExitFailure;
    }
    if (code == kExitOk) {
      // --embed serves the API over msgpack-RPC on stdio; it needs no API key
      // and runs no agent turns, so it bypasses run_agent entirely.
      code = args->embed ? rpc_serve() : run_agent(args);
    }
  }
  tools_teardown();   // unref Lua tool callbacks while the state is still alive
  autocmd_teardown(); // likewise for autocmd callbacks
  mua_lua_teardown();
  options_free(); // release option copies set from init.lua
  if (!loop_close() && code == kExitOk) {
    code = kExitFailure;
  }
  http_global_cleanup();
  return code;
}

int main(int argc, char **argv)
{
  MuaArgs args;
  if (!parse_args(argc, argv, &args)) {
    print_usage(stderr);
    return kExitUsage;
  }
  if (args.help) {
    print_usage(stdout);
    return kExitOk;
  }
  if (args.version) {
    if (printf("mua %s\n", MUA_VERSION_STRING) < 0) {
      return kExitFailure;
    }
    return kExitOk;
  }
  // Streamed stdout may be a closed pipe (`mua -p ... | head`); EPIPE is
  // handled at the write site, not by signal death.
  if (signal(SIGPIPE, SIG_IGN) == SIG_ERR) {
    (void)fprintf(stderr, "mua: failed to ignore SIGPIPE\n");
    return kExitFailure;
  }
  log_init();
  return run(&args);
}
