#ifndef MUA_API_PRIVATE_HELPERS_H
#define MUA_API_PRIVATE_HELPERS_H

#include "mua/api/private/defs.h"

#define MUA_PRINTF_ATTR(fmt_idx, args_idx) __attribute__((format(printf, fmt_idx, args_idx)))

// Formats into an xmalloc'd err->msg, releasing any prior message. `err` must
// be non-NULL; this is the only way API errors are raised (never assert).
void api_set_error(Error *err, ErrorType type, const char *fmt, ...) MUA_PRINTF_ATTR(3, 4);
void api_clear_error(Error *err); // releases msg, resets to ERROR_INIT; idempotent

String cstr_to_string(const char *str); // xmalloc'd copy (NUL kept); NULL -> STRING_INIT
String cstr_as_string(const char *str); // zero-copy view; NULL -> STRING_INIT
void api_free_string(String str);

#define NIL ((Object){.type = kObjectTypeNil})
#define BOOLEAN_OBJ(b) ((Object){.type = kObjectTypeBoolean, .data.boolean = (b)})
#define INTEGER_OBJ(i) ((Object){.type = kObjectTypeInteger, .data.integer = (i)})
#define FLOAT_OBJ(f) ((Object){.type = kObjectTypeFloat, .data.floating = (f)})
#define STRING_OBJ(s) ((Object){.type = kObjectTypeString, .data.string = (s)})
#define ARRAY_OBJ(a) ((Object){.type = kObjectTypeArray, .data.array = (a)})
#define DICT_OBJ(d) ((Object){.type = kObjectTypeDict, .data.dict = (d)})

#endif // MUA_API_PRIVATE_HELPERS_H
