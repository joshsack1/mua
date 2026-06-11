#ifndef MUA_LOG_H
#define MUA_LOG_H

#include <stdbool.h>

typedef enum {
  kLogDebug = 0,
  kLogInfo,
  kLogWarn,
  kLogError,
} LogLevel;

// Trace logging to stderr, fully off unless MUA_LOG names a level
// (debug|info|warn|error). For user-facing diagnostics use fprintf(stderr)
// directly; this is for development tracing only.
void log_init(void);
bool log_enabled(LogLevel level);
void log_msg(LogLevel level, const char *fmt, ...) __attribute__((format(printf, 2, 3)));

#endif // MUA_LOG_H
