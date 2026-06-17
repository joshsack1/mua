#include "mua/autocmd.h"

#include <string.h>

#include "mua/api/private/helpers.h"
#include "mua/lua/ref.h"

// A flat capped array of registrations, scanned by event. The counts are tiny
// in practice (a handful of hooks), so a linear scan per dispatch is fine and
// keeps the store a single contiguous allocation-free singleton.
enum { kMaxAutocmds = 256 };

typedef struct {
  Integer id;
  AutocmdEvent event;
  LuaRef callback;
} AutocmdEntry;

static AutocmdEntry g_autocmds[kMaxAutocmds];
static size_t g_autocmd_count;
static Integer g_next_id = 1; // monotonic; never reused (a future del_autocmd)

// Indexed by AutocmdEvent; lengths let the match avoid a NUL assumption.
static const struct {
  const char *name;
  size_t len;
} kEventNames[kAutocmdEventCount] = {
  [kAutocmdSessionStart] = {"SessionStart", 12},
  [kAutocmdSessionEnd] = {"SessionEnd", 10},
  [kAutocmdToolPre] = {"ToolPre", 7},
  [kAutocmdToolPost] = {"ToolPost", 8},
  [kAutocmdStreamDelta] = {"StreamDelta", 11},
  [kAutocmdUserPromptPre] = {"UserPromptPre", 13},
};

int autocmd_event_from_name(String name)
{
  if (name.data == NULL) {
    return -1;
  }
  for (int event = 0; event < kAutocmdEventCount; event++) {
    if (name.size == kEventNames[event].len &&
        memcmp(name.data, kEventNames[event].name, name.size) == 0) {
      return event;
    }
  }
  return -1;
}

Integer autocmd_create(AutocmdEvent event, LuaRef callback, Error *err)
{
  if (g_autocmd_count >= kMaxAutocmds) {
    api_set_error(err, kErrorTypeValidation, "too many autocmds (max %d)", kMaxAutocmds);
    return 0;
  }
  Integer id = g_next_id++;
  g_autocmds[g_autocmd_count++] = (AutocmdEntry){.id = id, .event = event, .callback = callback};
  return id;
}

size_t autocmd_count(AutocmdEvent event)
{
  size_t n = 0;
  for (size_t i = 0; i < g_autocmd_count; i++) {
    if (g_autocmds[i].event == event) {
      n++;
    }
  }
  return n;
}

LuaRef autocmd_ref_at(AutocmdEvent event, size_t idx)
{
  size_t n = 0;
  for (size_t i = 0; i < g_autocmd_count; i++) {
    if (g_autocmds[i].event == event) {
      if (n == idx) {
        return g_autocmds[i].callback;
      }
      n++;
    }
  }
  return -1; // unreachable: callers iterate within autocmd_count (LUA_REFNIL otherwise)
}

void autocmd_clear(void)
{
  for (size_t i = 0; i < g_autocmd_count; i++) {
    mua_lua_unref(g_autocmds[i].callback);
    g_autocmds[i] = (AutocmdEntry){0};
  }
  g_autocmd_count = 0;
}

void autocmd_teardown(void)
{
  autocmd_clear();
}
