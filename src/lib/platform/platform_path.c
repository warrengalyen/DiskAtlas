/**
 * platform_path.c — Cross-platform path utilities.
 *
 * Compiled on all platforms as part of diskatlas_core.
 *
 * All paths are handled as UTF-8 strings internally. On Windows, use
 * da_utf8_to_wide() / da_wide_to_utf8() from da_platform.h when interfacing
 * with Win32 APIs that require wide strings.
 *
 * Implements (public, DISKATLAS_API):
 *   da_path_normalize()
 *   da_path_is_absolute()
 *   da_path_join()
 */

#include "diskatlas_storage.h"

#include <stddef.h>
#include <string.h>

/* -------------------------------------------------------------------------- */
/* da_path_normalize                                                          */
/* -------------------------------------------------------------------------- */

DISKATLAS_API void da_path_normalize(char *path, size_t len) {
  if (!path || len == 0) {
    return;
  }

  /* On Windows, convert backslashes to forward slashes first. */
#if defined(_WIN32)
  for (size_t i = 0; path[i] != '\0'; i++) {
    if (path[i] == '\\') {
      path[i] = '/';
    }
  }
#endif

  /* Collapse consecutive '/' into a single '/', except for a leading "//"
   * which is significant on Windows (UNC prefix "//server/share").
   * Strategy: write head; skip duplicates in read head. */
  size_t write = 0;
  size_t read  = 0;
  size_t slen  = strlen(path);

  /* Preserve a leading "//" (UNC / network paths on Windows). */
  int preserve_double_slash = 0;
#if defined(_WIN32)
  if (slen >= 2 && path[0] == '/' && path[1] == '/') {
    preserve_double_slash = 1;
  }
#endif

  while (read < slen) {
    char c = path[read];
    if (c == '/' && write > 0) {
      /* Already wrote a separator; skip additional consecutive slashes,
       * but not when we're preserving the leading "//". */
      if (path[write - 1] == '/' && !(preserve_double_slash && write == 2)) {
        read++;
        continue;
      }
    }
    if (write < len - 1) {
      path[write++] = c;
    }
    read++;
  }
  path[write] = '\0';
  (void)len; /* already bounded above */
}

/* -------------------------------------------------------------------------- */
/* da_path_is_absolute                                                        */
/* -------------------------------------------------------------------------- */

DISKATLAS_API int da_path_is_absolute(const char *path) {
  if (!path || path[0] == '\0') {
    return 0;
  }

  /* POSIX absolute: leading '/' or '//' */
  if (path[0] == '/') {
    return 1;
  }

#if defined(_WIN32)
  /* Windows UNC: starts with "\\" or "//" */
  if ((path[0] == '\\' && path[1] == '\\') ||
      (path[0] == '/'  && path[1] == '/')) {
    return 1;
  }

  /* Windows drive-letter absolute: "C:\" or "C:/" or "C:" (bare drive root) */
  if (((path[0] >= 'A' && path[0] <= 'Z') ||
       (path[0] >= 'a' && path[0] <= 'z')) &&
      path[1] == ':') {
    return 1;
  }
#endif

  return 0;
}

/* -------------------------------------------------------------------------- */
/* da_path_join                                                               */
/* -------------------------------------------------------------------------- */

DISKATLAS_API int da_path_join(char *out, size_t out_sz,
                               const char *base, const char *rel) {
  if (!out || out_sz == 0) {
    return -1;
  }
  out[0] = '\0';

  if (!base) {
    base = "";
  }
  if (!rel) {
    rel = "";
  }

  /* If rel is absolute, it replaces base entirely. */
  if (da_path_is_absolute(rel)) {
    size_t rlen = strlen(rel);
    if (rlen >= out_sz) {
      return -1;
    }
    memcpy(out, rel, rlen + 1);
    da_path_normalize(out, out_sz);
    return 0;
  }

  size_t blen = strlen(base);
  size_t rlen = strlen(rel);

  /* Determine whether we need to insert a separator. */
  int need_sep = (blen > 0) &&
                 (base[blen - 1] != '/' && base[blen - 1] != '\\') &&
                 (rlen > 0);

  size_t total = blen + (need_sep ? 1 : 0) + rlen;
  if (total >= out_sz) {
    return -1;
  }

  memcpy(out, base, blen);
  if (need_sep) {
    out[blen] = '/';
  }
  memcpy(out + blen + (need_sep ? 1 : 0), rel, rlen);
  out[total] = '\0';

  da_path_normalize(out, out_sz);
  return 0;
}
