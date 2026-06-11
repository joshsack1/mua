# Vendored: cJSON

- Upstream: https://github.com/DaveGamble/cJSON
- Version: v1.7.18 (tag), files `cJSON.c`, `cJSON.h`, `LICENSE` taken verbatim
- License: MIT (see LICENSE)
- Local modifications: **none** — vendored verbatim; update by replacing the
  files from a newer pinned tag, never by patching in place.
- Build: compiled as its own CMake target (`cjson`) without mua's warning
  flags; `CJSON_NESTING_LIMIT=64` is defined project-wide (overriding the
  `#ifndef` default of 1000) so cJSON's parse recursion has a small, statically
  known depth bound — that is mua's code-safety compliance mechanism for this
  library's input-driven recursion.
- Excluded from `make lint` and `make format` (both operate on `src/mua` only).
