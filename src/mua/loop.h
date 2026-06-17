#ifndef MUA_LOOP_H
#define MUA_LOOP_H

#include <stdbool.h>

#include <uv.h>

// The event loop singleton (documented global #1): one loop per process,
// initialized from main before any handles exist.
bool loop_init(void);
uv_loop_t *loop_get(void); // valid between loop_init and loop_close
int loop_run(void);        // uv_run(UV_RUN_DEFAULT)
int loop_run_nowait(void); // one non-blocking pass; flushes stale SIGINTs between turns
int loop_run_once(void);   // uv_run(UV_RUN_ONCE): block for one event (await one request)
void loop_stop(void);
bool loop_close(void); // false (and a log line) if handles leaked

// First SIGINT invokes `cb` (the HTTP layer registers in-flight request
// cancellation); a second SIGINT — or the first, with no callback set —
// stops the loop outright. Setting a callback resets the SIGINT count.
typedef void (*LoopInterruptCb)(void *userdata);
void loop_set_interrupt_cb(LoopInterruptCb cb, void *userdata);

#endif // MUA_LOOP_H
