#include "mua/api/private/helpers.h"

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
