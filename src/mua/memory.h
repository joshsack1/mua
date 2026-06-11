#ifndef MUA_MEMORY_H
#define MUA_MEMORY_H

#include <stdbool.h>
#include <stddef.h>

// Allocation wrappers that abort on OOM and never return NULL (nvim memory.c
// pattern). All internal allocation goes through these.
void *xmalloc(size_t size);
void *xcalloc(size_t count, size_t size);
void *xrealloc(void *ptr, size_t size);
char *xstrdup(const char *str);
char *xstrndup(const char *str, size_t n);
void *xmemdup(const void *data, size_t len);
void xfree(void *ptr);

// Bump allocator over chained heap blocks. Every allocation is 16-byte
// aligned; arena_finish frees all blocks and leaves the arena reusable.
typedef struct arena_block ArenaBlock;

typedef struct {
  ArenaBlock *current;
  size_t offset;
} Arena;

#define ARENA_INIT ((Arena){.current = NULL, .offset = 0})

void *arena_alloc(Arena *arena, size_t size);
void arena_finish(Arena *arena);

// Growable byte buffer with a hard cap. `max` bounds total size (code-safety:
// every accumulator is bounded); buf_append refuses growth past it. Capacity
// is kept across buf_reset so steady-state reuse does not allocate.
typedef struct {
  char *data;
  size_t size;
  size_t cap;
  size_t max;
} Buf;

void buf_init(Buf *buf, size_t max);
bool buf_append(Buf *buf, const char *bytes, size_t len);
void buf_reset(Buf *buf);
void buf_free(Buf *buf);

#endif // MUA_MEMORY_H
