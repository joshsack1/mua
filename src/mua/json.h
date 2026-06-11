#ifndef MUA_JSON_H
#define MUA_JSON_H

#include <stdbool.h>
#include <stdint.h>

#include <cjson/cJSON.h>

#include "mua/api/private/defs.h"

// Boundary rule: cJSON is a core-internal idiom. A cJSON* may flow freely
// between core modules (http, provider, session) but must NEVER appear in a
// src/mua/api/ signature or cross into Lua — the future API layer converts
// cJSON <-> Object in exactly one place.
//
// Tree-walk rule: model-controlled JSON is traversed by bounded direct path
// access (the getters below) or iteratively over child->next sibling lists
// with an explicit stack capped at depth 64 (== CJSON_NESTING_LIMIT, so no
// deeper tree can exist). Recursion over parsed JSON is forbidden outside
// cJSON's own internally-capped parser.

// Routes cJSON allocation through xmalloc/xfree so OOM aborts loudly and
// uniformly. Call once from main before any parse.
void json_init(void);

// Parses `doc` (need not be NUL-terminated). Rejects doc.size > max_size
// BEFORE parsing; max_size is always explicit at the call site. Returns NULL
// with a Validation error on oversize, malformed input, or depth > 64.
cJSON *json_parse(String doc, size_t max_size, Error *err);

// Typed getters: NULL/false when the key is missing or the type mismatches
// (`out` untouched). Callers aggregate one contextual Error themselves.
const char *json_get_cstr(const cJSON *obj, const char *key); // borrowed from the tree
bool json_get_int(const cJSON *obj, const char *key, int64_t *out);
bool json_get_bool(const cJSON *obj, const char *key, bool *out);
cJSON *json_get_obj(const cJSON *obj, const char *key);
cJSON *json_get_arr(const cJSON *obj, const char *key);

// Builders: thin sugar over cJSON_Add*. Allocation aborts on OOM via the
// hooks, so misuse aside there are no failure paths; misuse asserts.
cJSON *json_new_obj(void);
void json_add_str(cJSON *obj, const char *key, String val); // embedded NUL truncates
void json_add_cstr(cJSON *obj, const char *key, const char *val);
void json_add_int(cJSON *obj, const char *key, int64_t val);
void json_add_bool(cJSON *obj, const char *key, bool val);
cJSON *json_add_arr(cJSON *obj, const char *key);

String json_print(const cJSON *node); // unformatted; caller xfrees .data
void json_free(cJSON *node);          // NULL-safe

#endif // MUA_JSON_H
