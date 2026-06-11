#include "mua/log.h"

#include <assert.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { kLogOff = kLogError + 1 };

// Documented singleton state: set once by log_init, read-only afterwards.
static int g_log_threshold = kLogOff;

void log_init(void)
{
  static const struct {
    const char *name;
    int level;
  } levels[] = {
    {"debug", kLogDebug},
    {"info", kLogInfo},
    {"warn", kLogWarn},
    {"error", kLogError},
  };
  g_log_threshold = kLogOff;
  const char *env = getenv("MUA_LOG");
  if (env == NULL) {
    return;
  }
  for (size_t i = 0; i < sizeof(levels) / sizeof(levels[0]); i++) {
    if (strcmp(env, levels[i].name) == 0) {
      g_log_threshold = levels[i].level;
      return;
    }
  }
}

bool log_enabled(LogLevel level)
{
  return (int)level >= g_log_threshold;
}

void log_msg(LogLevel level, const char *fmt, ...)
{
  static const char *const tags[] = {"DBG", "INF", "WRN", "ERR"};
  assert(level >= kLogDebug && level <= kLogError); // internal misuse only
  if (!log_enabled(level)) {
    return;
  }
  va_list args;
  va_start(args, fmt);
  // Trace output is best-effort by design: a full or closed stderr must
  // never turn logging into control flow. The repo's tolerated unchecked
  // stderr writes live here.
  (void)fprintf(stderr, "mua: %s ", tags[level]);
  (void)vfprintf(stderr, fmt, args);
  (void)fputc('\n', stderr);
  va_end(args);
}
