# Warning and sanitizer flags carried by an INTERFACE target so they apply
# only to mua's own code, never to vendored or system code.
add_library(mua_flags INTERFACE)

target_compile_options(mua_flags INTERFACE
  -Wall
  -Wextra
  -Wpedantic
  -Werror
  -Wshadow
  -Wvla
  -Wstrict-prototypes
  -Wmissing-prototypes
  -Wwrite-strings
  -Wdouble-promotion)

if(ENABLE_SANITIZERS)
  target_compile_options(mua_flags INTERFACE
    -fsanitize=address,undefined
    -fno-omit-frame-pointer
    -fno-sanitize-recover=all)
  target_link_options(mua_flags INTERFACE -fsanitize=address,undefined)
endif()
