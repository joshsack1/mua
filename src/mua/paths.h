#ifndef MUA_PATHS_H
#define MUA_PATHS_H

#include <stdbool.h>

#include "mua/api/private/defs.h"

// XDG path resolution. All returned strings are xmalloc'd; callers xfree.
// NULL means the location is unresolvable (no override, no XDG var, no HOME).
char *paths_config_dir(void);  // $MUA_CONFIG_DIR | $XDG_CONFIG_HOME/mua | ~/.config/mua
char *paths_state_dir(void);   // $MUA_STATE_DIR | $XDG_STATE_HOME/mua | ~/.local/state/mua
char *paths_runtime_dir(void); // $MUA_RUNTIME | compiled-in MUA_RUNTIME_PATH
char *paths_join(const char *a, const char *b); // "a/b", trailing slashes on a collapsed

// mkdir -p: creates `path` and any missing parents with mode 0700 (XDG state
// is private); existing directories are untouched (never chmod'd). Returns
// false with a Validation error when a non-directory is in the way, an
// Exception error on other syscall failures.
bool paths_ensure_dir(const char *path, Error *err);

#endif // MUA_PATHS_H
