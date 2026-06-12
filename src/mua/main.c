#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <cjson/cJSON.h>

#include "auto/versiondef.h"
#include "mua/api/private/helpers.h"
#include "mua/http.h"
#include "mua/json.h"
#include "mua/log.h"
#include "mua/loop.h"
#include "mua/lua/state.h"
#include "mua/provider/openrouter.h"

enum {
  kExitOk = 0,
  kExitFailure = 1,
  kExitUsage = 64,
  kExitInterrupted = 130,
};

typedef struct {
  bool version;
  bool help;
  const char *prompt;
} MuaArgs;

typedef struct {
  OpenrouterStream *stream; // NULLed when a terminal callback fires
  int exit_code;
  bool wrote_any;
  bool last_was_newline;
  bool interrupted;
  bool failed_write;
} PromptRun;

static void print_usage(FILE *stream)
{
  // Failing to print usage is unrecoverable and irrelevant to the exit path.
  (void)fprintf(stream, "Usage: mua [--version] [--help] [-p TEXT]\n");
}

static bool parse_args(int argc, char **argv, MuaArgs *out)
{
  *out = (MuaArgs){0};
  for (int i = 1; i < argc; i++) {
    const char *arg = argv[i];
    if (strcmp(arg, "--version") == 0) {
      out->version = true;
    } else if (strcmp(arg, "-h") == 0 || strcmp(arg, "--help") == 0) {
      out->help = true;
    } else if (strcmp(arg, "-p") == 0 || strcmp(arg, "--prompt") == 0) {
      if (i + 1 >= argc) {
        (void)fprintf(stderr, "mua: missing argument for %s\n", arg);
        return false;
      }
      i++;
      out->prompt = argv[i];
    } else {
      // Diagnostic on a failing path; nothing to do if stderr is gone.
      (void)fprintf(stderr, "mua: unknown argument: %s\n", arg);
      return false;
    }
  }
  return true;
}

static void prompt_on_text(void *ud, const String *text)
{
  PromptRun *run = ud;
  if (run->failed_write || text->size == 0) {
    return;
  }
  if (fwrite(text->data, 1, text->size, stdout) != text->size || fflush(stdout) != 0) {
    // stdout is gone (EPIPE under `mua -p ... | head`): stop streaming.
    run->failed_write = true;
    run->exit_code = kExitFailure;
    openrouter_cancel(run->stream);
    return;
  }
  run->wrote_any = true;
  run->last_was_newline = (text->data[text->size - 1] == '\n');
}

static void prompt_on_done(void *ud, cJSON *message, const String *finish_reason,
                           int64_t completion_tokens)
{
  PromptRun *run = ud;
  json_free(message); // ownership transferred by on_done; the agent loop will keep it
  log_msg(kLogInfo, "stream done: finish_reason=%.*s completion_tokens=%lld",
          (int)finish_reason->size, finish_reason->data != NULL ? finish_reason->data : "",
          (long long)completion_tokens);
  if (run->wrote_any && !run->last_was_newline && !run->failed_write) {
    (void)fputc('\n', stdout); // cosmetic trailing newline; best-effort
  }
  if (!run->failed_write) {
    run->exit_code = kExitOk;
  }
  run->stream = NULL;
}

static void prompt_on_error(void *ud, const Error *err)
{
  PromptRun *run = ud;
  run->stream = NULL;
  if (run->interrupted) {
    run->exit_code = kExitInterrupted;
    return;
  }
  if (run->failed_write) {
    return; // self-inflicted cancel; the exit code is already set
  }
  (void)fprintf(stderr, "mua: %s\n", err->msg != NULL ? err->msg : "request failed");
  run->exit_code = kExitFailure;
}

static void prompt_interrupt(void *ud)
{
  PromptRun *run = ud;
  run->interrupted = true;
  openrouter_cancel(run->stream); // NULL-safe after a terminal callback
}

static int run_prompt(const char *prompt)
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
  PromptRun run = {.exit_code = kExitFailure};
  cJSON *messages = cJSON_CreateArray();
  cJSON *message = cJSON_CreateObject();
  json_add_cstr(message, "role", "user");
  json_add_cstr(message, "content", prompt);
  cJSON_AddItemToArray(messages, message);
  OpenrouterOpts opts = {.messages = messages, .api_key = api_key};
  OpenrouterCallbacks cbs = {
    .on_text = prompt_on_text,
    .on_done = prompt_on_done,
    .on_error = prompt_on_error,
  };
  run.stream = openrouter_stream(http, &opts, &cbs, &run, &err);
  json_free(messages); // borrowed only for the call: the body is already printed
  if (run.stream == NULL) {
    (void)fprintf(stderr, "mua: %s\n", err.msg != NULL ? err.msg : "request failed");
    api_clear_error(&err);
  } else {
    loop_set_interrupt_cb(prompt_interrupt, &run);
    (void)loop_run(); // returns once the stream (and its handles) finished
    loop_set_interrupt_cb(NULL, NULL);
  }
  http_client_close(http);
  (void)loop_run(); // drain the client's deferred teardown
  return run.exit_code;
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
    // Broken user config is nonfatal by contract; sourcing only fails on
    // internal errors.
    if (!mua_lua_source_init()) {
      (void)fprintf(stderr, "mua: failed to source init.lua\n");
      code = kExitFailure;
    }
    if (code == kExitOk && args->prompt != NULL) {
      code = run_prompt(args->prompt);
    }
  }
  mua_lua_teardown();
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
