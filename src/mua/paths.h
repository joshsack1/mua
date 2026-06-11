#ifndef MUA_PATHS_H
#define MUA_PATHS_H

// XDG path resolution. All returned strings are xmalloc'd; callers xfree.
// NULL means the location is unresolvable (no override, no XDG var, no HOME).
char *paths_config_dir(void);  // $MUA_CONFIG_DIR | $XDG_CONFIG_HOME/mua | ~/.config/mua
char *paths_state_dir(void);   // $MUA_STATE_DIR | $XDG_STATE_HOME/mua | ~/.local/state/mua
char *paths_runtime_dir(void); // $MUA_RUNTIME | compiled-in MUA_RUNTIME_PATH
char *paths_join(const char *a, const char *b); // "a/b", trailing slashes on a collapsed

#endif // MUA_PATHS_H
