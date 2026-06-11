#include "mua/memory.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum {
  kArenaBlockSize = 4096,
  kArenaAlign = 16,
};

struct arena_block {
  ArenaBlock *prev;
  size_t size;
  char data[];
};

static void oom_abort(const char *what, size_t size)
{
  // Last words on a fatal path; nothing to recover if stderr is gone too.
  (void)fprintf(stderr, "mua: out of memory: %s(%zu)\n", what, size);
  abort();
}

void *xmalloc(size_t size)
{
  void *ptr = malloc(size ? size : 1);
  if (ptr == NULL) {
    oom_abort("xmalloc", size);
  }
  return ptr;
}

void *xcalloc(size_t count, size_t size)
{
  void *ptr = calloc(count ? count : 1, size ? size : 1);
  if (ptr == NULL) {
    // calloc reports count*size overflow as failure, which lands here too.
    oom_abort("xcalloc", size);
  }
  return ptr;
}

void *xrealloc(void *ptr, size_t size)
{
  void *out = realloc(ptr, size ? size : 1);
  if (out == NULL) {
    oom_abort("xrealloc", size);
  }
  return out;
}

char *xstrdup(const char *str)
{
  return xmemdup(str, strlen(str) + 1);
}

char *xstrndup(const char *str, size_t n)
{
  size_t len = strnlen(str, n);
  char *out = xmalloc(len + 1);
  memcpy(out, str, len);
  out[len] = '\0';
  return out;
}

void *xmemdup(const void *data, size_t len)
{
  void *out = xmalloc(len ? len : 1);
  if (len > 0) {
    memcpy(out, data, len);
  }
  return out;
}

void xfree(void *ptr)
{
  free(ptr);
}

void *arena_alloc(Arena *arena, size_t size)
{
  size_t aligned = (size + (kArenaAlign - 1)) & ~(size_t)(kArenaAlign - 1);
  if (aligned < size) {
    oom_abort("arena_alloc", size);
  }
  if (arena->current == NULL || aligned > arena->current->size - arena->offset) {
    size_t block_size = aligned > kArenaBlockSize ? aligned : kArenaBlockSize;
    ArenaBlock *block = xmalloc(sizeof(ArenaBlock) + block_size);
    block->prev = arena->current;
    block->size = block_size;
    arena->current = block;
    arena->offset = 0;
  }
  void *ptr = arena->current->data + arena->offset;
  arena->offset += aligned;
  return ptr;
}

void arena_finish(Arena *arena)
{
  ArenaBlock *block = arena->current;
  // Bounded by the number of blocks this arena allocated.
  while (block != NULL) {
    ArenaBlock *prev = block->prev;
    xfree(block);
    block = prev;
  }
  arena->current = NULL;
  arena->offset = 0;
}

void buf_init(Buf *buf, size_t max)
{
  *buf = (Buf){.data = NULL, .size = 0, .cap = 0, .max = max};
}

bool buf_append(Buf *buf, const char *bytes, size_t len)
{
  if (len == 0) {
    return true;
  }
  if (len > buf->max - buf->size) {
    // size <= max is an invariant, so the subtraction cannot wrap.
    return false;
  }
  size_t needed = buf->size + len;
  if (needed > buf->cap) {
    size_t new_cap = buf->cap ? buf->cap : 64;
    // Bounded: doubles toward `needed`, clamping at max (needed <= max here).
    while (new_cap < needed) {
      if (new_cap > buf->max / 2) {
        new_cap = buf->max;
      } else {
        new_cap *= 2;
      }
    }
    buf->data = xrealloc(buf->data, new_cap);
    buf->cap = new_cap;
  }
  memcpy(buf->data + buf->size, bytes, len);
  buf->size = needed;
  return true;
}

void buf_reset(Buf *buf)
{
  buf->size = 0;
}

void buf_free(Buf *buf)
{
  xfree(buf->data);
  buf_init(buf, buf->max);
}
