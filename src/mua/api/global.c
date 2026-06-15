#include "mua/api/global.h"

#include "mua/options.h"

// Thin wrappers over the options store: the API layer is the stable surface a
// Lua bridge (and, later, an --embed RPC dispatcher) targets, kept separate
// from the storage mechanism it forwards to.

void mua_set_option(String name, Object value, Error *err)
{
  options_set(name, value, err);
}

Object mua_get_option(String name, Error *err)
{
  return options_get(name, err);
}
