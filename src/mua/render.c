#include "mua/render.h"

#include <string.h>

#include "mua/memory.h"

// SGR escape sequences (ghostty: truecolor + standard SGR assumed). Attribute-
// specific off-codes (22/23/39), not a blanket reset, so a span never clobbers
// an enclosing one. SGR 22 clears both bold and faint intensity.
#define SGR_BOLD_ON "\x1b[1m"
#define SGR_BOLD_OFF "\x1b[22m"
#define SGR_ITALIC_ON "\x1b[3m"
#define SGR_ITALIC_OFF "\x1b[23m"
#define SGR_CODE_ON "\x1b[36m"  // cyan foreground
#define SGR_CODE_OFF "\x1b[39m" // default foreground
#define SGR_DIM_ON "\x1b[2m"
#define SGR_DIM_OFF "\x1b[22m"

static bool emit(Buf *out, const char *s)
{
  return buf_append(out, s, strlen(s));
}

// Index of the first `ch` in p[from, n), or n if absent. Bounded by n.
static size_t find_char(const char *p, size_t from, size_t n, char ch)
{
  for (size_t i = from; i < n; i++) {
    if (p[i] == ch) {
      return i;
    }
  }
  return n;
}

// One emphasis run at *ip (the marker is '*'). Run length 1 (italic) or 2
// (bold), capped at 2. Forward-searches for a closing run of at least that
// length; on a miss the opener is emitted literally and the cursor advances
// past it (no backtracking). Interior bytes are emitted verbatim -- one level
// deep, no nested inline scan.
static bool emphasis(const char *p, size_t *ip, size_t n, Buf *out)
{
  size_t i = *ip;
  size_t run = (i + 1 < n && p[i + 1] == '*') ? 2 : 1;
  size_t scan = i + run;
  size_t close = n;
  while (scan < n) {
    if (p[scan] != '*') {
      scan++;
      continue;
    }
    size_t k = scan;
    while (k < n && p[k] == '*') {
      k++;
    }
    if (k - scan >= run) {
      close = scan;
      break;
    }
    scan = k; // insufficient run; skip it (forward only)
  }
  if (close == n) {
    *ip = i + run;
    return buf_append(out, p + i, run); // unmatched: opener is literal
  }
  bool ok = emit(out, run == 2 ? SGR_BOLD_ON : SGR_ITALIC_ON) &&
            buf_append(out, p + i + run, close - (i + run)) &&
            emit(out, run == 2 ? SGR_BOLD_OFF : SGR_ITALIC_OFF);
  *ip = close + run;
  return ok;
}

// Inline span scan over one line's content: a single left-to-right pass, no
// backtracking. Handles backslash escapes, single-backtick code spans, and '*'
// emphasis; every other byte (including '_') is literal.
static bool inline_pass(const char *p, size_t n, Buf *out)
{
  size_t i = 0;
  while (i < n) {
    char c = p[i];
    if (c == '\\' && i + 1 < n) {
      if (!buf_append(out, p + i + 1, 1)) {
        return false;
      }
      i += 2;
    } else if (c == '`') {
      size_t j = find_char(p, i + 1, n, '`');
      if (j < n) {
        if (!emit(out, SGR_CODE_ON) || !buf_append(out, p + i + 1, j - (i + 1)) ||
            !emit(out, SGR_CODE_OFF)) {
          return false;
        }
        i = j + 1;
      } else {
        if (!buf_append(out, &c, 1)) {
          return false;
        }
        i += 1;
      }
    } else if (c == '*') {
      if (!emphasis(p, &i, n, out)) {
        return false;
      }
    } else {
      if (!buf_append(out, &c, 1)) {
        return false;
      }
      i += 1;
    }
  }
  return true;
}

// True if the line (after up to leading spaces) opens/closes a fenced block.
static bool is_fence(const char *p, size_t n)
{
  size_t i = 0;
  while (i < n && p[i] == ' ') {
    i++;
  }
  if (n - i < 3) {
    return false;
  }
  return (p[i] == '`' && p[i + 1] == '`' && p[i + 2] == '`') ||
         (p[i] == '~' && p[i + 1] == '~' && p[i + 2] == '~');
}

// Length of a leading blockquote/list marker (incl. its trailing space) to pass
// through unstyled, so a bullet '*' is not mistaken for emphasis. 0 if none. No
// indentation/renumbering/nesting -- the bytes are emitted as-is.
static size_t block_prefix(const char *p, size_t n)
{
  size_t i = 0;
  while (i < n && (p[i] == ' ' || p[i] == '\t')) {
    i++;
  }
  if (i >= n) {
    return 0;
  }
  if (p[i] == '>') {
    return (i + 1 < n && p[i + 1] == ' ') ? i + 2 : i + 1;
  }
  if ((p[i] == '-' || p[i] == '*' || p[i] == '+') && i + 1 < n && p[i + 1] == ' ') {
    return i + 2;
  }
  size_t j = i;
  while (j < n && p[j] >= '0' && p[j] <= '9') {
    j++;
  }
  if (j > i && j + 1 < n && (p[j] == '.' || p[j] == ')') && p[j + 1] == ' ') {
    return j + 2;
  }
  return 0;
}

// A normal (non-fence) line: ATX heading -> bold (hashes stripped, remainder
// not re-scanned); else pass any block-marker prefix through and inline-style
// the remainder.
static bool normal_line(const char *p, size_t n, Buf *out)
{
  size_t i = 0;
  while (i < n && i < 3 && p[i] == ' ') {
    i++;
  }
  size_t hashes = 0;
  while (i + hashes < n && hashes < 6 && p[i + hashes] == '#') {
    hashes++;
  }
  if (hashes > 0 && (i + hashes == n || p[i + hashes] == ' ')) {
    size_t start = i + hashes;
    if (start < n && p[start] == ' ') {
      start++;
    }
    return emit(out, SGR_BOLD_ON) && buf_append(out, p + start, n - start) &&
           emit(out, SGR_BOLD_OFF);
  }
  size_t prefix = block_prefix(p, n);
  if (prefix > 0 && !buf_append(out, p, prefix)) {
    return false;
  }
  return inline_pass(p + prefix, n - prefix, out);
}

// Styles one complete line (the bytes in r->line, no '\n') into `out`.
static bool style_line(Renderer *r, const char *p, size_t n, Buf *out)
{
  if (is_fence(p, n)) {
    r->in_fence = !r->in_fence;
    return emit(out, SGR_DIM_ON) && buf_append(out, p, n) && emit(out, SGR_DIM_OFF);
  }
  if (r->in_fence) {
    return emit(out, SGR_CODE_ON) && buf_append(out, p, n) && emit(out, SGR_CODE_OFF);
  }
  return normal_line(p, n, out);
}

void render_init(Renderer *r)
{
  buf_init(&r->line, kRenderLineMax);
  r->in_fence = false;
  r->line_overflowed = false;
  r->ended_newline = true; // neutral; only consulted once wrote_any is true
}

bool render_feed(Renderer *r, String in, Buf *out)
{
  for (size_t i = 0; i < in.size; i++) {
    char c = in.data[i];
    if (c == '\n') {
      if (!r->line_overflowed && !style_line(r, r->line.data, r->line.size, out)) {
        return false;
      }
      buf_reset(&r->line);
      r->line_overflowed = false;
      if (!buf_append(out, "\n", 1)) {
        return false;
      }
      r->ended_newline = true;
      continue;
    }
    if (r->line_overflowed) {
      if (!buf_append(out, &c, 1)) { // styling abandoned for this monster line
        return false;
      }
      r->ended_newline = false;
      continue;
    }
    if (!buf_append(&r->line, &c, 1)) {
      // Hit kRenderLineMax: flush what we have raw and continue raw to the next
      // '\n'. Memory stays bounded; only styling is sacrificed for this line.
      if (!buf_append(out, r->line.data, r->line.size) || !buf_append(out, &c, 1)) {
        return false;
      }
      buf_reset(&r->line);
      r->line_overflowed = true;
      r->ended_newline = false;
    }
  }
  return true;
}

bool render_flush(Renderer *r, Buf *out)
{
  if (!r->line_overflowed && r->line.size > 0) {
    if (!style_line(r, r->line.data, r->line.size, out)) {
      return false;
    }
    r->ended_newline = false; // emitted content with no trailing newline
  }
  buf_reset(&r->line);
  r->line_overflowed = false;
  r->in_fence = false;
  return true;
}

void render_free(Renderer *r)
{
  buf_free(&r->line);
}
