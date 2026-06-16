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

// Maximum Object nesting depth. Bounds the explicit-stack walks below (and,
// later, Lua<->Object marshaling) so no walk recurses over input-shaped depth,
// per json.h's tree-walk rule. Config/variable data is shallow; producers
// reject deeper input, so a tree exceeding this cannot reach the walks -- the
// bound is a can't-happen invariant there (asserted), not an input check.
enum { kMarshalDepthCap = 32 };

// Doubles represent every integer exactly up to 2^53; a number beyond that is
// kept as a Float rather than risk a lossy double<->int64 round-trip. Shared by
// every number-marshaling site (Lua<->Object in bridge.c, cJSON<->Object in
// json.c) so the int/float split stays identical across them.
#define MUA_EXACT_INT_MAX 9007199254740992.0 // 2^53

// Deep-copies `s` into a fresh allocation that keeps a trailing NUL (size
// excludes it; embedded NULs are preserved). NULL data -> STRING_INIT.
String api_string_dup(String s);

// Returns a fully owned deep copy of `*src`: scalars by value, String
// duplicated, Array/Dict given fresh backing storage copied element by element.
// Iterative (bounded explicit stack), never recursive. NULL src -> Nil.
Object api_copy_object(const Object *src);

// Frees every heap allocation owned by `*obj` -- String data, nested Array/Dict
// backing arrays, and Dict keys -- then resets it to Nil. Iterative; NULL-safe
// and idempotent on a Nil/zeroed Object. Not for arena-allocated trees.
void api_free_object(Object *obj);

#endif // MUA_API_PRIVATE_HELPERS_H
