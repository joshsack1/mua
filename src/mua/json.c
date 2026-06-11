#include "mua/json.h"

#include <assert.h>
#include <stddef.h>
#include <string.h>

#include <cjson/cJSON.h>

#include "mua/api/private/helpers.h"
#include "mua/memory.h"

void json_init(void)
{
  // Process-global, set once: cJSON can never observe a NULL allocation.
  cJSON_Hooks hooks = {.malloc_fn = xmalloc, .free_fn = xfree};
  cJSON_InitHooks(&hooks);
}

cJSON *json_parse(String doc, size_t max_size, Error *err)
{
  if (doc.data == NULL) {
    api_set_error(err, kErrorTypeValidation, "json: empty document");
    return NULL;
  }
  if (doc.size > max_size) {
    api_set_error(err, kErrorTypeValidation, "json: document of %zu bytes exceeds cap of %zu",
                  doc.size, max_size);
    return NULL;
  }
  const char *parse_end = NULL;
  // require_null_terminated=false: it would demand a readable '\0' after the
  // value, which a size-bounded slice does not have. Single-document
  // strictness is enforced below via parse_end instead.
  cJSON *node = cJSON_ParseWithLengthOpts(doc.data, doc.size, &parse_end, false);
  if (node == NULL) {
    size_t offset = 0;
    if (parse_end != NULL && parse_end >= doc.data) {
      offset = (size_t)(parse_end - doc.data);
    }
    api_set_error(err, kErrorTypeValidation, "json: parse failed near byte %zu", offset);
    return NULL;
  }
  // A document is exactly one JSON value: only whitespace (or one trailing
  // NUL from C-string callers) may follow it.
  const char *end = doc.data + doc.size;
  const char *cursor = parse_end;
  // Bounded by doc.size.
  while (cursor < end &&
         (*cursor == ' ' || *cursor == '\t' || *cursor == '\r' || *cursor == '\n')) {
    cursor++;
  }
  if (cursor < end && *cursor != '\0') {
    json_free(node);
    api_set_error(err, kErrorTypeValidation, "json: trailing bytes after document at offset %zu",
                  (size_t)(cursor - doc.data));
    return NULL;
  }
  return node;
}

const char *json_get_cstr(const cJSON *obj, const char *key)
{
  const cJSON *item = cJSON_GetObjectItemCaseSensitive(obj, key);
  return cJSON_IsString(item) ? item->valuestring : NULL;
}

bool json_get_int(const cJSON *obj, const char *key, int64_t *out)
{
  const cJSON *item = cJSON_GetObjectItemCaseSensitive(obj, key);
  if (!cJSON_IsNumber(item)) {
    return false;
  }
  *out = (int64_t)item->valuedouble;
  return true;
}

bool json_get_bool(const cJSON *obj, const char *key, bool *out)
{
  const cJSON *item = cJSON_GetObjectItemCaseSensitive(obj, key);
  if (!cJSON_IsBool(item)) {
    return false;
  }
  *out = cJSON_IsTrue(item);
  return true;
}

cJSON *json_get_obj(const cJSON *obj, const char *key)
{
  cJSON *item = cJSON_GetObjectItemCaseSensitive(obj, key);
  return cJSON_IsObject(item) ? item : NULL;
}

cJSON *json_get_arr(const cJSON *obj, const char *key)
{
  cJSON *item = cJSON_GetObjectItemCaseSensitive(obj, key);
  return cJSON_IsArray(item) ? item : NULL;
}

cJSON *json_new_obj(void)
{
  cJSON *obj = cJSON_CreateObject();
  assert(obj != NULL); // hooks abort on OOM; NULL means internal misuse
  return obj;
}

void json_add_str(cJSON *obj, const char *key, String val)
{
  // cJSON's C-string API cannot carry embedded NULs; our request bodies
  // never contain them (truncation documented at the declaration).
  char *tmp = xstrndup(val.data != NULL ? val.data : "", val.size);
  cJSON *item = cJSON_AddStringToObject(obj, key, tmp);
  assert(item != NULL);
  (void)item; // assert-only in debug; allocation cannot fail
  xfree(tmp);
}

void json_add_cstr(cJSON *obj, const char *key, const char *val)
{
  cJSON *item = cJSON_AddStringToObject(obj, key, val);
  assert(item != NULL);
  (void)item;
}

void json_add_int(cJSON *obj, const char *key, int64_t val)
{
  cJSON *item = cJSON_AddNumberToObject(obj, key, (double)val);
  assert(item != NULL);
  (void)item;
}

void json_add_bool(cJSON *obj, const char *key, bool val)
{
  cJSON *item = cJSON_AddBoolToObject(obj, key, val);
  assert(item != NULL);
  (void)item;
}

cJSON *json_add_arr(cJSON *obj, const char *key)
{
  cJSON *arr = cJSON_AddArrayToObject(obj, key);
  assert(arr != NULL);
  return arr;
}

String json_print(const cJSON *node)
{
  char *printed = cJSON_PrintUnformatted(node);
  if (printed == NULL) {
    // Unreachable with aborting hooks; kept so a misuse cannot crash callers.
    return STRING_INIT;
  }
  return (String){.data = printed, .size = strlen(printed)};
}

void json_free(cJSON *node)
{
  cJSON_Delete(node);
}
