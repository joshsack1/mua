#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "auto/versiondef.h"

enum {
  kExitOk = 0,
  kExitFailure = 1,
  kExitUsage = 64,
};

typedef struct {
  bool version;
  bool help;
} MuaArgs;

static void print_usage(FILE *stream)
{
  // Failing to print usage is unrecoverable and irrelevant to the exit path.
  (void)fprintf(stream, "Usage: mua [--version] [--help]\n");
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
    } else {
      // Diagnostic on a failing path; nothing to do if stderr is gone.
      (void)fprintf(stderr, "mua: unknown argument: %s\n", arg);
      return false;
    }
  }
  return true;
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
  // Nothing to do yet: startup wiring lands with the core modules.
  return kExitOk;
}
