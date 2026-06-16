#ifndef MUA_API_PRIVATE_DEFS_H
#define MUA_API_PRIVATE_DEFS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// API type vocabulary, mirroring nvim's src/nvim/api/private/defs.h.

// Marks the API level a function first appeared in (the API is append-only:
// deprecate, never repurpose). A compile-time no-op today -- a future dispatch
// generator reads it -- written after the parameter list, like nvim's
// FUNC_API_SINCE.
#ifndef FUNC_API_SINCE
#define FUNC_API_SINCE(x)
#endif

typedef int32_t handle_T;
typedef handle_T Session; // 0 means "current session", as 0 means current buffer in nvim

// A Lua function held in the registry via luaL_ref (nvim's LuaRef). Lua-only --
// a callback cannot be marshaled to an Object or over RPC, so it travels as its
// own parameter type, never an Object variant.
typedef int LuaRef;

typedef struct {
  char *data; // never assume NUL-termination
  size_t size;
} String;

typedef int64_t Integer;
typedef bool Boolean;
typedef double Float;

typedef enum {
  kErrorTypeNone = -1,
  kErrorTypeException,
  kErrorTypeValidation,
} ErrorType;

typedef struct {
  ErrorType type;
  char *msg; // xmalloc'd; released by api_clear_error
} Error;

#define ERROR_INIT ((Error){.type = kErrorTypeNone, .msg = NULL})
#define ERROR_SET(e) ((e)->type != kErrorTypeNone)
#define STRING_INIT ((String){.data = NULL, .size = 0})

typedef struct object Object;
typedef struct key_value_pair KeyValuePair;

typedef struct {
  Object *items;
  size_t size;
  size_t capacity;
} Array;

typedef struct {
  KeyValuePair *items;
  size_t size;
  size_t capacity;
} Dict;

typedef enum {
  kObjectTypeNil = 0,
  kObjectTypeBoolean,
  kObjectTypeInteger,
  kObjectTypeFloat,
  kObjectTypeString,
  kObjectTypeArray,
  kObjectTypeDict,
  kObjectTypeSession,
} ObjectType;

struct object {
  ObjectType type;
  union {
    Boolean boolean;
    Integer integer;
    Float floating;
    String string;
    Array array;
    Dict dict;
  } data;
};

struct key_value_pair {
  String key;
  Object value;
};

#endif // MUA_API_PRIVATE_DEFS_H
