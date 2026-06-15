#include "mua/api/private/helpers.h"

#include <assert.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "mua/memory.h"

void api_set_error(Error *err, ErrorType type, const char *fmt, ...)
{
  va_list args;
  va_list args_copy;
  va_start(args, fmt);
  va_copy(args_copy, args);
  int len = vsnprintf(NULL, 0, fmt, args);
  va_end(args);

  char *msg = NULL;
  if (len < 0) {
    // Formatting itself failed; keep the raw format string as the message.
    msg = xstrdup(fmt);
  } else {
    msg = xmalloc((size_t)len + 1);
    // Cannot fail: identical arguments just succeeded in the sizing pass.
    (void)vsnprintf(msg, (size_t)len + 1, fmt, args_copy);
  }
  va_end(args_copy);

  xfree(err->msg);
  err->msg = msg;
  err->type = type;
}

void api_clear_error(Error *err)
{
  xfree(err->msg);
  *err = ERROR_INIT;
}

String cstr_to_string(const char *str)
{
  if (str == NULL) {
    return STRING_INIT;
  }
  size_t len = strlen(str);
  return (String){.data = xmemdup(str, len + 1), .size = len};
}

String cstr_as_string(const char *str)
{
  if (str == NULL) {
    return STRING_INIT;
  }
  // Zero-copy view: the const-stripping cast does not confer ownership or
  // mutability on callers; the data still belongs to `str`.
  return (String){.data = (char *)str, .size = strlen(str)};
}

void api_free_string(String str)
{
  xfree(str.data);
}

String api_string_dup(String s)
{
  if (s.data == NULL) {
    return STRING_INIT;
  }
  char *buf = xmalloc(s.size + 1);
  memcpy(buf, s.data, s.size);
  buf[s.size] = '\0';
  return (String){.data = buf, .size = s.size};
}

// Shallow-initializes `*dst` from `*src`: scalars/Nil by value, String
// duplicated, Array/Dict typed with a freshly allocated (zeroed) backing array
// sized to `src`. Container elements are left for the walk in api_copy_object.
static void copy_init(const Object *src, Object *dst)
{
  dst->type = src->type;
  switch (src->type) {
    case kObjectTypeString:
      dst->data.string = api_string_dup(src->data.string);
      break;
    case kObjectTypeArray: {
      size_t n = src->data.array.size;
      dst->data.array.size = n;
      dst->data.array.capacity = n;
      dst->data.array.items = (n > 0) ? xcalloc(n, sizeof(Object)) : NULL;
      break;
    }
    case kObjectTypeDict: {
      size_t n = src->data.dict.size;
      dst->data.dict.size = n;
      dst->data.dict.capacity = n;
      dst->data.dict.items = (n > 0) ? xcalloc(n, sizeof(KeyValuePair)) : NULL;
      break;
    }
    default:
      dst->data = src->data; // scalar / Nil: the union value is the whole copy
      break;
  }
}

typedef struct {
  const Object *src; // source container being copied
  Object *dst;       // its fresh copy: backing array allocated, elements pending
  size_t i;          // next child index to copy
} CopyFrame;

Object api_copy_object(const Object *src)
{
  if (src == NULL) {
    return NIL;
  }
  Object root;
  copy_init(src, &root);
  if (root.type != kObjectTypeArray && root.type != kObjectTypeDict) {
    return root; // scalar or String: fully copied, no children to walk
  }
  CopyFrame stack[kMarshalDepthCap];
  int depth = 0;
  stack[depth++] = (CopyFrame){.src = src, .dst = &root, .i = 0};
  while (depth > 0) {
    CopyFrame *f = &stack[depth - 1];
    bool is_array = f->src->type == kObjectTypeArray;
    size_t n = is_array ? f->src->data.array.size : f->src->data.dict.size;
    if (f->i >= n) {
      depth--;
      continue;
    }
    size_t idx = f->i++;
    const Object *csrc = NULL;
    Object *cdst = NULL;
    if (is_array) {
      csrc = &f->src->data.array.items[idx];
      cdst = &f->dst->data.array.items[idx];
    } else {
      f->dst->data.dict.items[idx].key = api_string_dup(f->src->data.dict.items[idx].key);
      csrc = &f->src->data.dict.items[idx].value;
      cdst = &f->dst->data.dict.items[idx].value;
    }
    copy_init(csrc, cdst);
    if (cdst->type == kObjectTypeArray || cdst->type == kObjectTypeDict) {
      assert(depth < kMarshalDepthCap); // producers cap depth; deeper cannot occur
      stack[depth++] = (CopyFrame){.src = csrc, .dst = cdst, .i = 0};
    }
  }
  return root;
}

typedef struct {
  Object *node; // a container (Array or Dict) being drained
  size_t i;     // next child index to free
} FreeFrame;

void api_free_object(Object *obj)
{
  if (obj == NULL) {
    return;
  }
  if (obj->type == kObjectTypeString) {
    api_free_string(obj->data.string);
    *obj = NIL;
    return;
  }
  if (obj->type != kObjectTypeArray && obj->type != kObjectTypeDict) {
    *obj = NIL; // scalar / Nil: nothing owned
    return;
  }
  FreeFrame stack[kMarshalDepthCap];
  int depth = 0;
  stack[depth++] = (FreeFrame){.node = obj, .i = 0};
  while (depth > 0) {
    FreeFrame *f = &stack[depth - 1];
    bool is_array = f->node->type == kObjectTypeArray;
    size_t n = is_array ? f->node->data.array.size : f->node->data.dict.size;
    if (f->i < n) {
      size_t idx = f->i++;
      Object *child = NULL;
      if (is_array) {
        child = &f->node->data.array.items[idx];
      } else {
        api_free_string(f->node->data.dict.items[idx].key);
        child = &f->node->data.dict.items[idx].value;
      }
      if (child->type == kObjectTypeString) {
        api_free_string(child->data.string);
      } else if (child->type == kObjectTypeArray || child->type == kObjectTypeDict) {
        assert(depth < kMarshalDepthCap); // producers cap depth; deeper cannot occur
        stack[depth++] = (FreeFrame){.node = child, .i = 0};
      }
    } else {
      // All children released; free this container's backing array, then pop.
      if (is_array) {
        xfree(f->node->data.array.items);
      } else {
        xfree(f->node->data.dict.items);
      }
      depth--;
    }
  }
  *obj = NIL;
}
