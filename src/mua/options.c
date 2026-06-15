#include "mua/options.h"

#include <assert.h>
#include <string.h>

#include "mua/api/private/helpers.h"
#include "mua/memory.h"

// The built-in system prompt: the default when neither env nor init.lua sets
// one. Lives here (not the agent) so it is the option table's declared default
// and `mua.o.system_prompt` reads it back when unset.
#define MUA_DEFAULT_SYSTEM_PROMPT                                                                  \
  "You are mua, a minimal coding agent. Use the provided tools to read, "                          \
  "write, and edit files and to run shell commands in the current working "                        \
  "directory. Keep responses brief."

typedef struct {
  const char *name;
  ObjectType type;
  const char *str_default; // string options: default text (NULL means Nil)
  Integer int_default;     // integer options: default value
  Integer int_min;         // integer options: inclusive range
  Integer int_max;
} OptionDef;

static const OptionDef option_defs[] = {
    {"system_prompt", kObjectTypeString, MUA_DEFAULT_SYSTEM_PROMPT, 0, 0, 0},
    {"model", kObjectTypeString, NULL, 0, 0, 0},
    {"step_cap", kObjectTypeInteger, NULL, MUA_STEP_CAP_MAX, MUA_STEP_CAP_MIN, MUA_STEP_CAP_MAX},
};

enum { kOptionCount = (int)(sizeof option_defs / sizeof option_defs[0]) };

// Indices into option_defs[]/g_options[]; MUST match the table order above.
// The typed getters assert their binding so a reorder fails loudly in debug.
enum { kOptSystemPrompt = 0, kOptModel, kOptStepCap };

typedef struct {
  bool is_set;
  Object value; // owns its String data when set and type is string
} OptionSlot;

// Documented mutable singleton #3 (see options.h). Bounded: one slot per
// compile-time option, never grows.
static OptionSlot g_options[kOptionCount];

// A NUL-terminated deep copy: keeps the length (the API round-trip may carry
// embedded NULs) and a trailing NUL (the agent uses these as C strings).
static String string_dup(String s)
{
  if (s.data == NULL) {
    return STRING_INIT;
  }
  char *buf = xmalloc(s.size + 1);
  memcpy(buf, s.data, s.size);
  buf[s.size] = '\0';
  return (String){.data = buf, .size = s.size};
}

static Object clone_object(Object o)
{
  if (o.type == kObjectTypeString) {
    return (Object){.type = kObjectTypeString, .data.string = string_dup(o.data.string)};
  }
  return o; // scalar / Nil: a by-value copy is a full copy
}

static const char *type_name(ObjectType type)
{
  switch (type) {
  case kObjectTypeString:
    return "a string";
  case kObjectTypeInteger:
    return "an integer";
  case kObjectTypeBoolean:
    return "a boolean";
  case kObjectTypeFloat:
    return "a number";
  default:
    return "a value";
  }
}

static int option_index(String name)
{
  for (int i = 0; i < kOptionCount; i++) {
    if (strlen(option_defs[i].name) == name.size && name.data != NULL &&
        memcmp(option_defs[i].name, name.data, name.size) == 0) {
      return i;
    }
  }
  return -1;
}

static void clear_slot(int idx)
{
  if (g_options[idx].is_set && g_options[idx].value.type == kObjectTypeString) {
    api_free_string(g_options[idx].value.data.string);
  }
  g_options[idx].is_set = false;
  g_options[idx].value = (Object){.type = kObjectTypeNil};
}

static Object default_object(const OptionDef *def)
{
  if (def->type == kObjectTypeString && def->str_default != NULL) {
    return (Object){.type = kObjectTypeString, .data.string = cstr_to_string(def->str_default)};
  }
  if (def->type == kObjectTypeInteger) {
    return (Object){.type = kObjectTypeInteger, .data.integer = def->int_default};
  }
  return (Object){.type = kObjectTypeNil}; // string option with no default (e.g. model)
}

void options_set(String name, Object value, Error *err)
{
  int idx = option_index(name);
  if (idx < 0) {
    api_set_error(err, kErrorTypeValidation, "unknown option '%.*s'", (int)name.size,
                  name.data != NULL ? name.data : "");
    return;
  }
  const OptionDef *def = &option_defs[idx];
  if (value.type != def->type) {
    api_set_error(err, kErrorTypeValidation, "option '%s' expects %s", def->name,
                  type_name(def->type));
    return;
  }
  if (def->type == kObjectTypeInteger &&
      (value.data.integer < def->int_min || value.data.integer > def->int_max)) {
    api_set_error(err, kErrorTypeValidation, "option '%s' out of range [%lld, %lld]", def->name,
                  (long long)def->int_min, (long long)def->int_max);
    return;
  }
  clear_slot(idx); // free any prior value before overwriting
  g_options[idx].value = clone_object(value);
  g_options[idx].is_set = true;
}

Object options_get(String name, Error *err)
{
  int idx = option_index(name);
  if (idx < 0) {
    api_set_error(err, kErrorTypeValidation, "unknown option '%.*s'", (int)name.size,
                  name.data != NULL ? name.data : "");
    return (Object){.type = kObjectTypeNil};
  }
  if (g_options[idx].is_set) {
    return clone_object(g_options[idx].value);
  }
  return default_object(&option_defs[idx]);
}

const char *options_system_prompt(void)
{
  assert(strcmp(option_defs[kOptSystemPrompt].name, "system_prompt") == 0);
  const OptionSlot *slot = &g_options[kOptSystemPrompt];
  return slot->is_set ? slot->value.data.string.data : option_defs[kOptSystemPrompt].str_default;
}

const char *options_model_borrow(void)
{
  assert(strcmp(option_defs[kOptModel].name, "model") == 0);
  const OptionSlot *slot = &g_options[kOptModel];
  return slot->is_set ? slot->value.data.string.data : NULL;
}

int options_step_cap(void)
{
  assert(strcmp(option_defs[kOptStepCap].name, "step_cap") == 0);
  const OptionSlot *slot = &g_options[kOptStepCap];
  return slot->is_set ? (int)slot->value.data.integer : (int)option_defs[kOptStepCap].int_default;
}

void options_free(void)
{
  for (int i = 0; i < kOptionCount; i++) {
    clear_slot(i);
  }
}
