#include <ctype.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>
#include <wctype.h>

#include "diskatlas_wildcard.h"

static const char *utf8_basename_ptr(const char *path) {
  const char *base = path ? path : "";
  for (const char *q = base; *q; q++) {
    if (*q == '/' || *q == '\\') {
      base = q + 1;
    }
  }
  return base;
}

static bool str_is_ascii(const char *s) {
  if (!s) {
    return true;
  }
  for (const unsigned char *p = (const unsigned char *)s; *p; p++) {
    if (*p >= 0x80u) {
      return false;
    }
  }
  return true;
}

static bool ascii_contains_ci(const char *hay, const char *needle) {
  if (!needle || !needle[0]) {
    return true;
  }
  if (!hay) {
    return false;
  }
  size_t nl = strlen(needle);
  for (; *hay; hay++) {
    size_t i;
    for (i = 0; i < nl; i++) {
      if (tolower((unsigned char)hay[i]) != tolower((unsigned char)needle[i])) {
        break;
      }
    }
    if (i == nl) {
      return true;
    }
  }
  return false;
}

static size_t utf8_next(const char *s, wchar_t *out_cp) {
  mbstate_t st;
  memset(&st, 0, sizeof(st));
  wchar_t wc = 0;
  size_t n = mbrtowc(&wc, s, 32, &st);
  if (n == 0) {
    *out_cp = 0;
    return 0;
  }
  if (n == (size_t)-1 || n == (size_t)-2) {
    *out_cp = (wchar_t)(unsigned char)s[0];
    return 1;
  }
  *out_cp = wc;
  return n;
}

static size_t utf8_encode(wchar_t cp, char *dst, size_t dst_cap) {
  mbstate_t st;
  memset(&st, 0, sizeof(st));
  if (dst_cap == 0) {
    return 0;
  }
  size_t n = wcrtomb(dst, cp, &st);
  if (n == (size_t)-1) {
    dst[0] = (char)(unsigned char)cp;
    return 1;
  }
  return n;
}

static wchar_t fold_wchar(wchar_t wc) {
  return towlower(wc);
}

/** Growable folded UTF-8 buffer for match. */
typedef struct {
  wchar_t *wcs;
  size_t len;
  size_t cap;
} fold_buf_t;

static bool fold_buf_append_utf8(fold_buf_t *b, const char *utf8) {
  const char *p = utf8 ? utf8 : "";
  while (*p) {
    wchar_t cp = 0;
    size_t adv = utf8_next(p, &cp);
    if (adv == 0) {
      break;
    }
    wchar_t fc = fold_wchar(cp);
    if (b->len + 1 >= b->cap) {
      size_t nc = b->cap ? b->cap * 2u : 64u;
      wchar_t *nw = (wchar_t *)realloc(b->wcs, nc * sizeof(wchar_t));
      if (!nw) {
        return false;
      }
      b->wcs = nw;
      b->cap = nc;
    }
    b->wcs[b->len++] = fc;
    p += adv;
  }
  if (b->len + 1 >= b->cap) {
    size_t nc = b->cap ? b->cap * 2u : 64u;
    wchar_t *nw = (wchar_t *)realloc(b->wcs, nc * sizeof(wchar_t));
    if (!nw) {
      return false;
    }
    b->wcs = nw;
    b->cap = nc;
  }
  b->wcs[b->len] = L'\0';
  return true;
}

static void fold_buf_clear(fold_buf_t *b) {
  free(b->wcs);
  b->wcs = NULL;
  b->len = 0;
  b->cap = 0;
}

static bool utf8_fold_to_wcs(const char *utf8, fold_buf_t *out) {
  out->wcs = NULL;
  out->len = 0;
  out->cap = 0;
  return fold_buf_append_utf8(out, utf8);
}

static bool wcs_contains(const wchar_t *hay, const wchar_t *needle) {
  if (!needle || !needle[0]) {
    return true;
  }
  if (!hay) {
    hay = L"";
  }
  size_t nl = wcslen(needle);
  for (const wchar_t *p = hay; *p; p++) {
    size_t i;
    for (i = 0; i < nl && p[i] != L'\0'; i++) {
      if (p[i] != needle[i]) {
        break;
      }
    }
    if (i == nl) {
      return true;
    }
  }
  return false;
}

static bool wild_match_folded_wcs(const wchar_t *pat, const wchar_t *str, int depth) {
  if (depth++ > 10000) {
    return false;
  }
  if (*pat == L'\0') {
    return *str == L'\0';
  }
  if (*pat == L'*') {
    const wchar_t *next_pat = pat + 1;
    if (*next_pat == L'\0') {
      return true;
    }
    for (;;) {
      if (wild_match_folded_wcs(next_pat, str, depth)) {
        return true;
      }
      if (*str == L'\0') {
        return false;
      }
      str++;
    }
  }
  if (*pat == L'?') {
    if (*str == L'\0') {
      return false;
    }
    return wild_match_folded_wcs(pat + 1, str + 1, depth);
  }
  if (*str == L'\0') {
    return false;
  }
  if (*pat != *str) {
    return false;
  }
  return wild_match_folded_wcs(pat + 1, str + 1, depth);
}

DISKATLAS_API bool diskatlas_filter_has_wildcard(const char *filter_utf8) {
  if (!filter_utf8) {
    return false;
  }
  const char *p = filter_utf8;
  while (*p) {
    wchar_t cp = 0;
    size_t adv = utf8_next(p, &cp);
    if (adv == 0) {
      break;
    }
    if (cp == L'*' || cp == L'?') {
      return true;
    }
    p += adv;
  }
  return false;
}

static bool utf8_folded_matches_filter(const char *hay_utf8, const char *filter_utf8) {
  if (!filter_utf8 || !filter_utf8[0]) {
    return true;
  }
  const char *hay = hay_utf8 ? hay_utf8 : "";

  if (!diskatlas_filter_has_wildcard(filter_utf8) && str_is_ascii(hay) && str_is_ascii(filter_utf8)) {
    return ascii_contains_ci(hay, filter_utf8);
  }

  fold_buf_t hay_f = {0};
  fold_buf_t fi_f = {0};
  bool ok = false;
  if (!utf8_fold_to_wcs(hay, &hay_f) || !utf8_fold_to_wcs(filter_utf8, &fi_f)) {
    goto done;
  }
  if (diskatlas_filter_has_wildcard(filter_utf8)) {
    ok = wild_match_folded_wcs(fi_f.wcs, hay_f.wcs, 0);
  } else {
    ok = wcs_contains(hay_f.wcs, fi_f.wcs);
  }

done:
  fold_buf_clear(&hay_f);
  fold_buf_clear(&fi_f);
  return ok;
}

DISKATLAS_API bool diskatlas_utf8_matches_filter(const char *hay_utf8, const char *filter_utf8,
                                                 bool basename_only) {
  if (basename_only) {
    return utf8_folded_matches_filter(utf8_basename_ptr(hay_utf8), filter_utf8);
  }
  return utf8_folded_matches_filter(hay_utf8, filter_utf8);
}
