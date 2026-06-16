#include "mua/api/global.h"

#include "mua/api/private/helpers.h"
#include "mua/json.h"
#include "mua/options.h"
#include "mua/tools.h"
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

void mua_register_tool(String name, String description, Object schema, Boolean mutating,
                       LuaRef callback, Error *err)
{
  // The one place a schema crosses Object -> cJSON (the json.h boundary rule):
  // a Dict becomes the wire schema, Nil defaults to an empty-object schema, and
  // anything else is rejected. The signature stays cJSON-free.
  cJSON *schema_json;
  if (schema.type == kObjectTypeNil) {
    schema_json = json_new_obj();
    json_add_cstr(schema_json, "type", "object");
    cJSON_AddItemToObject(schema_json, "properties", json_new_obj());
  } else if (schema.type == kObjectTypeDict) {
    schema_json = object_to_cjson(&schema);
  } else {
    api_set_error(err, kErrorTypeValidation, "tool schema must be a table");
    return;
  }
  tools_register(name, description, schema_json, mutating, callback, err);
  if (ERROR_SET(err)) {
    json_free(schema_json); // registration refused it; release our converted copy
  }
}
