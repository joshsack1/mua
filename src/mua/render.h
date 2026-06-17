#ifndef MUA_RENDER_H
#define MUA_RENDER_H

#include <stdbool.h>

#include "mua/api/private/defs.h" // String
#include "mua/memory.h"           // Buf

// Streaming markdown -> ANSI (SGR) renderer for the interactive terminal.
// PURE: it never touches stdout or any file descriptor. Each feed appends
// styled bytes to a caller-owned `out` Buf; the caller drains+resets it (so the
// fwrite/EPIPE logic lives in exactly one place, and a feed is trivially
// unit-testable: bytes in, styled bytes out). Disabled by default -- main.c
// only drives it when `mua.o.markdown` is set AND stdout is a TTY, so piped
// output stays byte-identical.
//
// GOVERNING INVARIANT (the line that forecloses TUI-engine complexity):
//   The renderer NEVER measures terminal width, NEVER moves the cursor, and
//   NEVER re-touches a line it has already emitted. Its only output verb is
//   "append bytes to a Buf." Any feature needing one of those three is rejected
//   by construction -- which is also why setext headings (===/--- underlines)
//   and any lookback feature are impossible here, not merely unimplemented.
//
// Line-buffered: input accumulates into a per-line Buf; a completed line (at a
// '\n', or the tail at flush) is styled as a whole and emitted. So a markdown
// marker split across stream chunks is a non-issue -- styling only ever sees a
// complete line. Cross-line state is a single flag (in_fence). Every styled
// span opened on a line is closed on that same line, so nothing is ever left
// open across lines (flush needs no defensive reset).
//
// Targets ghostty / libghostty only: truecolor + SGR + italics assumed, no
// terminfo, no capability detection, no 8/16-color fallback.
//
// Element set (lean core): fenced code blocks (``` / ~~~, body verbatim in the
// code color; language tag on the delimiter is not parsed), inline code spans
// (single backtick), ATX headings (#..###### -> bold, hashes stripped), bold
// (**) and italic (*), backslash escapes, and blockquote/list markers passed
// through as plain bytes (content still styled; no indent/renumber/nesting).
//
// DELIBERATELY NOT a marker: '_' / '__'. Underscore emphasis needs CommonMark
// flanking rules to avoid mangling intraword underscores (my_var, __init__);
// those rules are exactly the complexity this module refuses, so '*' is the
// only emphasis marker and '_' is literal text.
//
// HARD NO -- do not add (each needs width math, an external lexer, lookback, or
// a repaint, all banned above): syntax highlighting inside fences, language-
// aware anything, tables, word-wrap/reflow, nested-list indentation, links of
// any kind (incl. OSC 8), images, reference/autolinks, setext headings,
// thematic breaks, strikethrough, HTML passthrough, entity decoding.

enum { kRenderLineMax = 1 << 20 }; // per-line accumulator cap; overflow flushes raw

typedef struct {
  Buf line;             // the current incomplete input line (never holds a '\n')
  bool in_fence;        // inside a ``` / ~~~ fenced code block
  bool line_overflowed; // current line passed the cap -> emit raw, no styling
  bool ended_newline;   // last byte appended to `out` was '\n' (owns the
                        // caller's cosmetic-trailing-newline decision)
} Renderer;

// Zeroes state; no allocation until the first feed.
void render_init(Renderer *r);

// Consumes `in` (one provider chunk, any byte boundary) and appends styled
// output to `out`. Returns false only if `out` hit its own cap -- the caller
// treats that identically to an fwrite failure.
bool render_feed(Renderer *r, String in, Buf *out);

// End of turn: styles+emits any pending partial line (no trailing '\n') and
// updates ended_newline. After this the renderer is back to start-of-line state.
bool render_flush(Renderer *r, Buf *out);

// Releases the line accumulator.
void render_free(Renderer *r);

#endif // MUA_RENDER_H
