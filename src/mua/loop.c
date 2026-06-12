#include "mua/loop.h"

#include <assert.h>
#include <signal.h>
#include <stddef.h>

#include <uv.h>

#include "mua/log.h"

static struct {
  uv_loop_t loop;
  uv_signal_t sigint;
  bool initialized;
  int sigint_count;
  LoopInterruptCb interrupt_cb;
  void *interrupt_ud;
} g_loop;

static void sigint_cb(uv_signal_t *handle, int signum)
{
  (void)handle; // fixed libuv callback signature
  (void)signum;
  g_loop.sigint_count++;
  if (g_loop.interrupt_cb != NULL && g_loop.sigint_count == 1) {
    g_loop.interrupt_cb(g_loop.interrupt_ud);
    return;
  }
  uv_stop(&g_loop.loop);
}

bool loop_init(void)
{
  if (g_loop.initialized) {
    return true;
  }
  if (uv_loop_init(&g_loop.loop) != 0) {
    return false;
  }
  if (uv_signal_init(&g_loop.loop, &g_loop.sigint) != 0) {
    // Best-effort unwind on a failing init path.
    (void)uv_loop_close(&g_loop.loop);
    return false;
  }
  if (uv_signal_start(&g_loop.sigint, sigint_cb, SIGINT) != 0) {
    uv_close((uv_handle_t *)&g_loop.sigint, NULL);
    (void)uv_run(&g_loop.loop, UV_RUN_NOWAIT); // deliver the close callback
    (void)uv_loop_close(&g_loop.loop);
    return false;
  }
  // The watcher must not keep an otherwise-idle loop alive.
  uv_unref((uv_handle_t *)&g_loop.sigint);
  g_loop.sigint_count = 0;
  g_loop.interrupt_cb = NULL;
  g_loop.interrupt_ud = NULL;
  g_loop.initialized = true;
  return true;
}

uv_loop_t *loop_get(void)
{
  assert(g_loop.initialized); // internal misuse only; callers init first
  return &g_loop.loop;
}

int loop_run(void)
{
  return uv_run(&g_loop.loop, UV_RUN_DEFAULT);
}

int loop_run_nowait(void)
{
  return uv_run(&g_loop.loop, UV_RUN_NOWAIT);
}

void loop_stop(void)
{
  uv_stop(&g_loop.loop);
}

bool loop_close(void)
{
  if (!g_loop.initialized) {
    return true;
  }
  uv_close((uv_handle_t *)&g_loop.sigint, NULL);
  int rc = UV_EBUSY;
  // Bounded drain: each pass runs pending close callbacks; well-behaved
  // teardown needs one or two passes, the cap guards against a leak loop.
  for (int i = 0; i < 16; i++) {
    rc = uv_loop_close(&g_loop.loop);
    if (rc != UV_EBUSY) {
      break;
    }
    (void)uv_run(&g_loop.loop, UV_RUN_NOWAIT); // only close callbacks remain
  }
  g_loop.initialized = false;
  if (rc != 0) {
    log_msg(kLogError, "event loop closed with leaked handles: %s", uv_strerror(rc));
    return false;
  }
  return true;
}

void loop_set_interrupt_cb(LoopInterruptCb cb, void *userdata)
{
  g_loop.interrupt_cb = cb;
  g_loop.interrupt_ud = userdata;
  g_loop.sigint_count = 0;
}
