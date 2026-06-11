#include "mua/lua/state.h"

#include <assert.h>
#include <stddef.h>
#include <stdio.h>
#include <sys/stat.h>

#include <lauxlib.h>
#include <lua.h>
#include <lualib.h>

#include "mua/memory.h"
#include "mua/paths.h"

static lua_State *g_lua = NULL;

static void prepend_runtime_path(lua_State *lstate, const char *runtime_dir)
{
  lua_getglobal(lstate, "package");
  lua_getfield(lstate, -1, "path");
  const char *old_path = lua_tostring(lstate, -1);
  lua_pushfstring(lstate, "%s/lua/?.lua;%s/lua/?/init.lua;%s", runtime_dir, runtime_dir,
                  old_path != NULL ? old_path : "");
  lua_setfield(lstate, -3, "path");
  lua_pop(lstate, 2);
}

bool mua_lua_init(void)
{
  if (g_lua != NULL) {
    return true;
  }
  lua_State *lstate = luaL_newstate();
  if (lstate == NULL) {
    return false;
  }
  luaL_openlibs(lstate);
  char *runtime_dir = paths_runtime_dir();
  if (runtime_dir == NULL) {
    lua_close(lstate);
    return false;
  }
  prepend_runtime_path(lstate, runtime_dir);
  xfree(runtime_dir);
  g_lua = lstate;
  return true;
}

lua_State *mua_lua_state(void)
{
  assert(g_lua != NULL); // internal misuse only; callers init first
  return g_lua;
}

static int traceback_handler(lua_State *lstate)
{
  const char *msg = lua_tostring(lstate, 1);
  luaL_traceback(lstate, lstate, msg, 1);
  return 1;
}

bool mua_lua_source_init(void)
{
  assert(g_lua != NULL);
  char *config_dir = paths_config_dir();
  if (config_dir == NULL) {
    return true; // nowhere to look: nothing to source
  }
  char *init_path = paths_join(config_dir, "init.lua");
  xfree(config_dir);

  struct stat st;
  if (stat(init_path, &st) != 0) {
    xfree(init_path);
    return true; // no init.lua: perfectly fine
  }

  lua_pushcfunction(g_lua, traceback_handler);
  int handler_idx = lua_gettop(g_lua);
  int rc = luaL_loadfile(g_lua, init_path);
  if (rc == 0) {
    rc = lua_pcall(g_lua, 0, 0, handler_idx);
  }
  if (rc != 0) {
    const char *err_msg = lua_tostring(g_lua, -1);
    // User-facing diagnostic on a nonfatal path; nothing to do if stderr fails.
    (void)fprintf(stderr, "mua: error in init.lua: %s\n",
                  err_msg != NULL ? err_msg : "(non-string error)");
    lua_pop(g_lua, 1);
  }
  lua_pop(g_lua, 1); // the traceback handler
  xfree(init_path);
  return true;
}

void mua_lua_teardown(void)
{
  if (g_lua != NULL) {
    lua_close(g_lua);
    g_lua = NULL;
  }
}
