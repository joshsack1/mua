#include "mua/api/global.h"

#include "mua/options.h"
#include "mua/variables.h"

// Thin wrappers over the C-side stores: the API layer is the stable surface a
// Lua bridge (and, later, an --embed RPC dispatcher) targets, kept separate
// from the storage mechanisms it forwards to.

void mua_set_option(String name, Object value, Error *err)
{
  options_set(name, value, err);
}

Object mua_get_option(String name, Error *err)
{
  return options_get(name, err);
}

void mua_set_var(String name, Object value, Error *err)
{
  variables_set(name, value, err);
}

Object mua_get_var(String name, Error *err)
{
  return variables_get(name, err);
}
