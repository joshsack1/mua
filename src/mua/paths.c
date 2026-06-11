#include "mua/paths.h"

#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#include "mua/memory.h"

static const char *nonempty_getenv(const char *name)
{
  const char *value = getenv(name);
  return (value != NULL && value[0] != '\0') ? value : NULL;
}

static char *xdg_dir(const char *override_var, const char *xdg_var, const char *home_suffix)
{
  const char *override = nonempty_getenv(override_var);
  if (override != NULL) {
    return xstrdup(override);
  }
  const char *xdg = nonempty_getenv(xdg_var);
  if (xdg != NULL) {
    return paths_join(xdg, "mua");
  }
  const char *home = nonempty_getenv("HOME");
  if (home == NULL) {
    return NULL;
  }
  return paths_join(home, home_suffix);
}

char *paths_config_dir(void)
{
  return xdg_dir("MUA_CONFIG_DIR", "XDG_CONFIG_HOME", ".config/mua");
}

char *paths_state_dir(void)
{
  return xdg_dir("MUA_STATE_DIR", "XDG_STATE_HOME", ".local/state/mua");
}

char *paths_runtime_dir(void)
{
  const char *override = nonempty_getenv("MUA_RUNTIME");
  if (override != NULL) {
    return xstrdup(override);
  }
  // Compile-time default; points at the source tree for dev builds.
  return xstrdup(MUA_RUNTIME_PATH);
}

char *paths_join(const char *a, const char *b)
{
  size_t len_a = strlen(a);
  while (len_a > 1 && a[len_a - 1] == '/') {
    len_a--;
  }
  // After trimming, `a` can still end in '/' only when it is the root.
  bool need_sep = !(len_a > 0 && a[len_a - 1] == '/');
  size_t len_b = strlen(b);
  char *out = xmalloc(len_a + (need_sep ? 1 : 0) + len_b + 1);
  memcpy(out, a, len_a);
  size_t pos = len_a;
  if (need_sep) {
    out[pos] = '/';
    pos++;
  }
  memcpy(out + pos, b, len_b);
  out[pos + len_b] = '\0';
  return out;
}
