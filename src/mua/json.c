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

// --- cJSON <-> Object marshaling (the boundary-rule converter) ----------------
// Iterative, depth-capped walks paralleling api_copy_object in helpers.c: one
// builds a heap Object tree from cJSON, the other the reverse. cJSON keys and
// string values are NUL-terminated C strings, so cstr_to_string is exact -- the
// only place an embedded NUL could be lost is object_to_cjson's String edge,
// which cannot reach a NUL-terminated cJSON string anyway.

static size_t cjson_child_count(const cJSON *node)
{
  size_t n = 0;
  for (const cJSON *c = node->child; c != NULL; c = c->next) {
    n++;
  }
  return n;
}

// Shallow-initializes `*dst` from cJSON `*src`: a scalar/null by value, a String
// copied, an Array/Dict typed with a freshly allocated (zeroed, hence Nil-filled)
// backing array. A Dict's keys are filled here; all container element values are
// left Nil for the walk to fill. Allocation aborts on OOM, so this cannot fail.
static void cjson_node_init(const cJSON *src, Object *dst)
{
  if (cJSON_IsString(src)) {
    *dst = STRING_OBJ(cstr_to_string(src->valuestring));
  } else if (cJSON_IsNumber(src)) {
    double num = src->valuedouble;
    if (num >= -MUA_EXACT_INT_MAX && num <= MUA_EXACT_INT_MAX && num == (double)(Integer)num) {
      *dst = INTEGER_OBJ((Integer)num);
    } else {
      *dst = FLOAT_OBJ(num);
    }
  } else if (cJSON_IsBool(src)) {
    *dst = BOOLEAN_OBJ(cJSON_IsTrue(src));
  } else if (cJSON_IsArray(src)) {
    size_t n = cjson_child_count(src);
    dst->type = kObjectTypeArray;
    dst->data.array.size = n;
    dst->data.array.capacity = n;
    dst->data.array.items = (n > 0) ? xcalloc(n, sizeof(Object)) : NULL; // zeroed -> Nil
  } else if (cJSON_IsObject(src)) {
    size_t n = cjson_child_count(src);
    KeyValuePair *items = (n > 0) ? xcalloc(n, sizeof(KeyValuePair)) : NULL;
    dst->type = kObjectTypeDict;
    dst->data.dict.size = n;
    dst->data.dict.capacity = n;
    dst->data.dict.items = items;
    if (items != NULL) { // n == 0 has no children to walk; guard makes that provable
      size_t i = 0;
      for (const cJSON *c = src->child; c != NULL; c = c->next) {
        items[i++].key = cstr_to_string(c->string); // value stays Nil for the walk
      }
    }
  } else {
    *dst = NIL; // cJSON null, or an unexpected type from a non-parser source
  }
}

typedef struct {
  const cJSON *src; // source container
  Object *dst;      // its fresh copy: backing allocated, values pending
  const cJSON *cur; // next child node to consume (parallels dst's index)
  size_t i;         // next child index to fill
} CjsonFrame;

bool cjson_to_object(const cJSON *node, Object *out, Error *err)
{
  Object root;
  cjson_node_init(node, &root);
  if (root.type != kObjectTypeArray && root.type != kObjectTypeDict) {
    *out = root; // scalar / String / Nil: fully built, no children to walk
    return true;
  }
  CjsonFrame stack[kMarshalDepthCap];
  int depth = 0;
  stack[depth++] = (CjsonFrame){.src = node, .dst = &root, .cur = node->child, .i = 0};
  while (depth > 0) {
    CjsonFrame *f = &stack[depth - 1];
    bool is_array = f->dst->type == kObjectTypeArray;
    size_t n = is_array ? f->dst->data.array.size : f->dst->data.dict.size;
    if (f->i >= n) {
      depth--;
      continue;
    }
    const cJSON *cs = f->cur;
    assert(cs != NULL); // cjson_node_init set dst's size from this same child chain
    Object *slot =
      is_array ? &f->dst->data.array.items[f->i] : &f->dst->data.dict.items[f->i].value;
    f->i++;
    f->cur = cs->next;
    // Reject (before allocating it) a container that would sit past the cap, so
    // the partial tree stays <= cap deep and api_free_object can unwind it.
    if ((cJSON_IsArray(cs) || cJSON_IsObject(cs)) && depth >= kMarshalDepthCap) {
      api_set_error(err, kErrorTypeValidation, "json nesting exceeds %d", kMarshalDepthCap);
      api_free_object(&root);
      return false;
    }
    cjson_node_init(cs, slot);
    if (slot->type == kObjectTypeArray || slot->type == kObjectTypeDict) {
      stack[depth++] = (CjsonFrame){.src = cs, .dst = slot, .cur = cs->child, .i = 0};
    }
  }
  *out = root;
  return true;
}

// Builds a leaf cJSON for a scalar/Nil, or an empty container the walk fills.
static cJSON *object_make_node(const Object *o)
{
  switch (o->type) {
    case kObjectTypeBoolean:
      return cJSON_CreateBool(o->data.boolean);
    case kObjectTypeInteger:
      return cJSON_CreateNumber((double)o->data.integer);
    case kObjectTypeFloat:
      return cJSON_CreateNumber(o->data.floating);
    case kObjectTypeString: {
      char *tmp = xstrndup(o->data.string.data != NULL ? o->data.string.data : "",
                           o->data.string.size);
      cJSON *str = cJSON_CreateString(tmp);
      xfree(tmp);
      return str;
    }
    case kObjectTypeArray:
      return cJSON_CreateArray();
    case kObjectTypeDict:
      return cJSON_CreateObject();
    default:
      return cJSON_CreateNull(); // Nil, Session, or unexpected
  }
}

typedef struct {
  const Object *src; // source Array/Dict
  cJSON *dst;        // its cJSON container being filled
  size_t i;          // next child index to emit
} ObjFrame;

cJSON *object_to_cjson(const Object *obj)
{
  if (obj == NULL) {
    return cJSON_CreateNull();
  }
  cJSON *root = object_make_node(obj);
  if (obj->type != kObjectTypeArray && obj->type != kObjectTypeDict) {
    return root; // scalar / String / Nil
  }
  ObjFrame stack[kMarshalDepthCap];
  int depth = 0;
  stack[depth++] = (ObjFrame){.src = obj, .dst = root, .i = 0};
  while (depth > 0) {
    ObjFrame *f = &stack[depth - 1];
    bool is_array = f->src->type == kObjectTypeArray;
    size_t n = is_array ? f->src->data.array.size : f->src->data.dict.size;
    if (f->i >= n) {
      depth--;
      continue;
    }
    size_t idx = f->i++;
    const Object *csrc =
      is_array ? &f->src->data.array.items[idx] : &f->src->data.dict.items[idx].value;
    cJSON *child = object_make_node(csrc);
    if (is_array) {
      cJSON_AddItemToArray(f->dst, child);
    } else {
      String key = f->src->data.dict.items[idx].key;
      char *kz = xstrndup(key.data != NULL ? key.data : "", key.size);
      cJSON_AddItemToObject(f->dst, kz, child); // copies the key; free our temp
      xfree(kz);
    }
    if (csrc->type == kObjectTypeArray || csrc->type == kObjectTypeDict) {
      assert(depth < kMarshalDepthCap); // producers cap depth; deeper cannot occur
      stack[depth++] = (ObjFrame){.src = csrc, .dst = child, .i = 0};
    }
  }
  return root;
}
