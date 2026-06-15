#include "mua/variables.h"

#include <string.h>

#include "mua/api/private/helpers.h"

// Documented mutable singleton #4 (see variables.h). Open-ended string->Object
// map as a flat array with linear lookup -- variable counts are small (config
// data), matching the options store's shape. Bounded by MUA_VAR_MAX.
typedef struct {
  String key;   // owned (api_string_dup'd); embedded NULs preserved
  Object value; // owned deep copy
} VarEntry;

static VarEntry g_vars[MUA_VAR_MAX];
static size_t g_var_count;

// Index of `name` in g_vars, or -1. Length-prefixed compare (keys and the
// lookup name may carry embedded NULs), matching options.c's option_index.
static int var_index(String name)
{
  for (size_t i = 0; i < g_var_count; i++) {
    if (g_vars[i].key.size == name.size && name.data != NULL &&
        memcmp(g_vars[i].key.data, name.data, name.size) == 0) {
      return (int)i;
    }
  }
  return -1;
}

// Frees entry `idx` and fills the hole with the last entry (order is not
// significant), keeping the array dense. The vacated last slot is emptied
// WITHOUT freeing -- its memory is now owned by `idx`.
static void remove_entry(int idx)
{
  api_free_string(g_vars[idx].key);
  api_free_object(&g_vars[idx].value);
  size_t last = g_var_count - 1;
  if ((size_t)idx != last) {
    g_vars[idx] = g_vars[last];
  }
  g_vars[last].key = STRING_INIT;
  g_vars[last].value = NIL;
  g_var_count--;
}

void variables_set(String name, Object value, Error *err)
{
  int idx = var_index(name);
  if (value.type == kObjectTypeNil) {
    if (idx >= 0) {
      remove_entry(idx); // mua.g.x = nil deletes; unset is a no-op
    }
    return;
  }
  if (idx >= 0) {
    api_free_object(&g_vars[idx].value); // overwrite: release the prior value
    g_vars[idx].value = api_copy_object(&value);
    return;
  }
  if (g_var_count == MUA_VAR_MAX) {
    api_set_error(err, kErrorTypeValidation, "too many variables (max %d)", MUA_VAR_MAX);
    return;
  }
  g_vars[g_var_count].key = api_string_dup(name);
  g_vars[g_var_count].value = api_copy_object(&value);
  g_var_count++;
}

Object variables_get(String name, Error *err)
{
  (void)err; // an unset variable is Nil, not an error (unlike options_get)
  int idx = var_index(name);
  if (idx < 0) {
    return NIL;
  }
  return api_copy_object(&g_vars[idx].value);
}

void variables_free(void)
{
  while (g_var_count > 0) {
    remove_entry((int)g_var_count - 1);
  }
}
