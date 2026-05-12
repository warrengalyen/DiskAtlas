#include <inttypes.h>
#include <limits.h>
#include <locale.h>
#include <stdio.h>
#include <string.h>

#include <glib.h>

#if defined(G_OS_WIN32)
#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <wchar.h>
#endif

#include "format_text.h"

static int da_bytes_decimal_places = 1;
static int da_bytes_display_format = DA_SIZE_DISPLAY_DYNAMIC;

void da_format_bytes_set_display_format(int format) {
  if (format < (int)DA_SIZE_DISPLAY_DYNAMIC || format > (int)DA_SIZE_DISPLAY_TB) {
    format = DA_SIZE_DISPLAY_DYNAMIC;
  }
  da_bytes_display_format = format;
}

int da_format_bytes_get_display_format(void) {
  return da_bytes_display_format;
}

void da_format_bytes_set_decimal_places(int places) {
  if (places < 0) {
    places = 0;
  } else if (places > 4) {
    places = 4;
  }
  da_bytes_decimal_places = places;
}

int da_format_bytes_get_decimal_places(void) {
  return da_bytes_decimal_places;
}

void da_format_bytes(uint64_t n, char *dst, size_t dstsz) {
  if (!dst || dstsz == 0) {
    return;
  }
  int d = da_bytes_decimal_places;
  switch (da_bytes_display_format) {
  case DA_SIZE_DISPLAY_BYTES: {
    char num[80];
    da_format_uint64_locale(n, num, sizeof(num));
    snprintf(dst, dstsz, "%s B", num);
    return;
  }
  case DA_SIZE_DISPLAY_KB:
    snprintf(dst, dstsz, "%.*f KB", d, (double)n / 1024.0);
    return;
  case DA_SIZE_DISPLAY_MB:
    snprintf(dst, dstsz, "%.*f MB", d, (double)n / (1024.0 * 1024.0));
    return;
  case DA_SIZE_DISPLAY_GB:
    snprintf(dst, dstsz, "%.*f GB", d, (double)n / (1024.0 * 1024.0 * 1024.0));
    return;
  case DA_SIZE_DISPLAY_TB:
    snprintf(dst, dstsz, "%.*f TB", d, (double)n / (1024.0 * 1024.0 * 1024.0 * 1024.0));
    return;
  case DA_SIZE_DISPLAY_DYNAMIC:
  default:
    break;
  }
  if (n < 1024ull) {
    snprintf(dst, dstsz, "%" PRIu64 " B", n);
    return;
  }
  if (n < 1024ull * 1024ull) {
    snprintf(dst, dstsz, "%.*f KiB", d, (double)n / 1024.0);
    return;
  }
  if (n < 1024ull * 1024ull * 1024ull) {
    snprintf(dst, dstsz, "%.*f MiB", d, (double)n / (1024.0 * 1024.0));
    return;
  }
  if (n < 1024ull * 1024ull * 1024ull * 1024ull) {
    snprintf(dst, dstsz, "%.*f GiB", d, (double)n / (1024.0 * 1024.0 * 1024.0));
    return;
  }
  snprintf(dst, dstsz, "%.*f TiB", d,
           (double)n / (1024.0 * 1024.0 * 1024.0 * 1024.0));
}

void da_format_bytes_with_pct(uint64_t bytes, uint64_t vol_total, char *dst, size_t dstsz) {
  if (!dst || dstsz == 0) {
    return;
  }
  char sz[80];
  da_format_bytes(bytes, sz, sizeof(sz));
  if (vol_total == 0u) {
    snprintf(dst, dstsz, "%s", sz);
    return;
  }
  double pct = 100.0 * (double)bytes / (double)vol_total;
  snprintf(dst, dstsz, "%s (%.1f%%)", sz, pct);
}

void da_format_pct_of_volume(uint64_t file_bytes, uint64_t vol_total, char *dst,
                             size_t dstsz) {
  if (!dst || dstsz == 0) {
    return;
  }
  if (vol_total == 0) {
    if (dstsz > 0) {
      dst[0] = '\0';
    }
    return;
  }
  double pct = 100.0 * (double)file_bytes / (double)vol_total;
  snprintf(dst, dstsz, "%.2f%%", pct);
}

void da_format_pct_progress_label(uint64_t file_bytes, uint64_t vol_total, char *dst, size_t dstsz) {
  if (!dst || dstsz == 0) {
    return;
  }
  if (vol_total == 0) {
    if (dstsz > 0) {
      dst[0] = '\0';
    }
    return;
  }
  double pct = 100.0 * (double)file_bytes / (double)vol_total;
  snprintf(dst, dstsz, "%.1f %%", pct);
}

void da_format_mtime_local(uint64_t unix_ns, char *dst, size_t dstsz) {
  if (!dst || dstsz == 0) {
    return;
  }
  if (unix_ns == 0) {
    if (dstsz > 0) {
      dst[0] = '\0';
    }
    return;
  }
  GDateTime *utc =
      g_date_time_new_from_unix_utc((gint64)(unix_ns / 1000000000ull));
  if (utc == NULL) {
    if (dstsz > 0) {
      dst[0] = '\0';
    }
    return;
  }
  GDateTime *loc = g_date_time_to_local(utc);
  gchar *s = g_date_time_format(loc, "%Y-%m-%d %H:%M:%S");
  if (s != NULL) {
    g_strlcpy(dst, s, dstsz);
    g_free(s);
  } else {
    if (dstsz > 0) {
      dst[0] = '\0';
    }
  }
  g_date_time_unref(loc);
  g_date_time_unref(utc);
}

void da_format_win32_attr_letters(uint32_t wa, char *dst, size_t dstsz) {
  if (!dst || dstsz == 0) {
    return;
  }
  if (wa == 0) {
    if (dstsz > 0) {
      dst[0] = '\0';
    }
    return;
  }
  char *p = dst;
  char *end = dst + dstsz;
#define PCH(c)                                                                 \
  do {                                                                         \
    if (p + 1 < end)                                                           \
      *p++ = (c);                                                              \
  } while (0)
  if (wa & 0x01u) {
    PCH('R');
  }
  if (wa & 0x02u) {
    PCH('H');
  }
  if (wa & 0x04u) {
    PCH('S');
  }
  if (wa & 0x10u) {
    PCH('D');
  }
  if (wa & 0x20u) {
    PCH('A');
  }
  if (wa & 0x400u) {
    PCH('L');
  }
#undef PCH
  if (p == dst) {
    snprintf(dst, dstsz, "0x%X", (unsigned)wa);
    return;
  }
  *p = '\0';
}

#if defined(G_OS_WIN32)
static UINT da_win32_parse_grouping_field(const wchar_t *gr) {
  if (gr == NULL || gr[0] == L'\0') {
    return 0u;
  }
  wchar_t *end = NULL;
  long v = wcstol(gr, &end, 10);
  (void)end;
  if (v >= 2L && v <= 9L) {
    return (UINT)v;
  }
  if (v == 0L) {
    return 0u;
  }
  return 3u;
}

/** User's regional thousands/decimal symbols; integers only (NumDigits = 0). */
static gboolean da_try_format_uint64_win32_user_default(uint64_t n, char *dst, size_t dstsz) {
  if (dstsz == 0) {
    return FALSE;
  }
  wchar_t wd[24];
  int nd = swprintf(wd, 24, L"%llu", (unsigned long long)n);
  if (nd <= 0 || nd >= 24) {
    return FALSE;
  }
  wchar_t wout[96];
  int cap = (int)(sizeof(wout) / sizeof(wout[0]));
  LCID lcid = GetUserDefaultLCID();

  wchar_t thous[8];
  wchar_t dec[8];
  wchar_t grpstr[16];
  if (GetLocaleInfoW(lcid, LOCALE_STHOUSAND, thous, (int)(sizeof(thous) / sizeof(thous[0]))) == 0) {
    thous[0] = L',';
    thous[1] = L'\0';
  }
  if (GetLocaleInfoW(lcid, LOCALE_SDECIMAL, dec, (int)(sizeof(dec) / sizeof(dec[0]))) == 0) {
    dec[0] = L'.';
    dec[1] = L'\0';
  }
  if (GetLocaleInfoW(lcid, LOCALE_SGROUPING, grpstr, (int)(sizeof(grpstr) / sizeof(grpstr[0]))) == 0) {
    grpstr[0] = L'3';
    grpstr[1] = L'\0';
  }

  NUMBERFMTW nf;
  nf.NumDigits = 0;
  nf.LeadingZero = 0;
  nf.Grouping = da_win32_parse_grouping_field(grpstr);
  nf.lpDecimalSep = dec;
  nf.lpThousandSep = thous;
  nf.NegativeOrder = 1;

  int nout = GetNumberFormatW(lcid, 0, wd, &nf, wout, cap);
  if (nout <= 0 || nout >= cap) {
    return FALSE;
  }
  wout[nout] = L'\0';
  gchar *u8 = g_utf16_to_utf8((const gunichar2 *)wout, -1, NULL, NULL, NULL);
  if (u8 == NULL) {
    return FALSE;
  }
  g_strlcpy(dst, u8, dstsz);
  g_free(u8);
  return TRUE;
}
#endif

void da_format_uint64_locale(uint64_t n, char *dst, size_t dstsz) {
  if (!dst || dstsz == 0) {
    return;
  }
#if defined(G_OS_WIN32)
  if (da_try_format_uint64_win32_user_default(n, dst, dstsz)) {
    return;
  }
#endif  /* FORMAT_TEXT_H */
  struct lconv *lc = localeconv();
  const char *sep = (lc != NULL && lc->thousands_sep != NULL) ? lc->thousands_sep : "";
  const char *grp = (lc != NULL && lc->grouping != NULL) ? lc->grouping : "";

  char rev[24];
  int len = 0;
  if (n == 0u) {
    rev[len++] = '0';
  } else {
    while (n > 0u && len < (int)sizeof(rev)) {
      rev[len++] = (char)('0' + (int)(n % 10u));
      n /= 10u;
    }
  }
  if (len >= (int)sizeof(rev)) {
    snprintf(dst, dstsz, "—");
    return;
  }

  /* No thousands separator: plain digits (C / POSIX default). */
  if (sep[0] == '\0') {
    size_t out = 0;
    for (int i = len - 1; i >= 0; i--) {
      if (out + 1 >= dstsz) {
        snprintf(dst, dstsz, "—");
        return;
      }
      dst[out++] = rev[i];
    }
    dst[out] = '\0';
    return;
  }

  /* grouping[0] == CHAR_MAX: no grouping (single block). */
  if (grp != NULL && (unsigned char)grp[0] == CHAR_MAX) {
    size_t out = 0;
    for (int i = len - 1; i >= 0; i--) {
      if (out + 1 >= dstsz) {
        snprintf(dst, dstsz, "—");
        return;
      }
      dst[out++] = rev[i];
    }
    dst[out] = '\0';
    return;
  }

  /* Empty grouping: no separators. */
  if (grp == NULL || grp[0] == '\0') {
    size_t out = 0;
    for (int i = len - 1; i >= 0; i--) {
      if (out + 1 >= dstsz) {
        snprintf(dst, dstsz, "—");
        return;
      }
      dst[out++] = rev[i];
    }
    dst[out] = '\0';
    return;
  }

  /* Chunk sizes from least significant toward most (POSIX grouping). */
  int chunks[24];
  int nc = 0;
  const char *gp = grp;
  int gprev = -1;
  int consumed = 0;
  while (consumed < len && nc < (int)(sizeof(chunks) / sizeof(chunks[0]))) {
    int sz;
    unsigned char gch = (unsigned char)*gp;
    if (gch == '\0') {
      sz = (gprev > 0) ? gprev : 3;
    } else if (gch == CHAR_MAX) {
      sz = len - consumed;
      gp++;
    } else if (gch == 0) {
      sz = (gprev > 0) ? gprev : 3;
    } else {
      gprev = (int)gch;
      sz = gprev;
      gp++;
    }
    if (sz <= 0) {
      sz = len - consumed;
    }
    if (sz > len - consumed) {
      sz = len - consumed;
    }
    chunks[nc++] = sz;
    consumed += sz;
  }
  if (consumed != len || nc <= 0) {
    snprintf(dst, dstsz, "—");
    return;
  }

  size_t out = 0;
  int hi = len - 1;
  for (int ci = nc - 1; ci >= 0; ci--) {
    if (ci < nc - 1) {
      size_t sl = strlen(sep);
      if (out + sl >= dstsz) {
        snprintf(dst, dstsz, "—");
        return;
      }
      memcpy(dst + out, sep, sl);
      out += sl;
    }
    for (int k = 0; k < chunks[ci]; k++) {
      if (hi < 0 || out + 1 >= dstsz) {
        snprintf(dst, dstsz, "—");
        return;
      }
      dst[out++] = rev[hi--];
    }
  }
  dst[out] = '\0';
}

static gboolean da_filter_has_wildcard(const char *filter) {
  if (!filter) {
    return FALSE;
  }
  for (const char *s = filter; *s; s = g_utf8_next_char(s)) {
    gunichar c = g_utf8_get_char(s);
    if (c == '*' || c == '?') {
      return TRUE;
    }
  }
  return FALSE;
}

static gboolean utf8_prefix_ci_match(const char *hay, const char *needle, const char **out_end) {
  const char *h = hay;
  const char *n = needle;
  while (*n) {
    if (*h == '\0') {
      return FALSE;
    }
    gunichar hc = g_utf8_get_char(h);
    gunichar nc = g_utf8_get_char(n);
    if (g_unichar_tolower(hc) != g_unichar_tolower(nc)) {
      return FALSE;
    }
    h = g_utf8_next_char(h);
    n = g_utf8_next_char(n);
  }
  if (out_end != NULL) {
    *out_end = h;
  }
  return TRUE;
}

/* Glob on UTF-8 case-folded strings; marks each *byte* of str for matched literals and ?. */
static gboolean wild_match_fold_mark(const char *pat, const char *str, gboolean *mark, const char *str_base,
                                     gsize mark_len, int depth) {
  if (depth++ > 10000) {
    return FALSE;
  }
  if (*pat == '\0') {
    return *str == '\0';
  }

  gunichar pc = g_utf8_get_char(pat);
  if (pc == '*') {
    const char *next_pat = g_utf8_next_char(pat);
    if (*next_pat == '\0') {
      return TRUE;
    }
    for (;;) {
      if (wild_match_fold_mark(next_pat, str, mark, str_base, mark_len, depth)) {
        return TRUE;
      }
      if (*str == '\0') {
        return FALSE;
      }
      str = g_utf8_next_char(str);
    }
  }
  if (pc == '?') {
    if (*str == '\0') {
      return FALSE;
    }
    const char *next_s = g_utf8_next_char(str);
    if (!wild_match_fold_mark(g_utf8_next_char(pat), next_s, mark, str_base, mark_len, depth)) {
      return FALSE;
    }
    for (const char *q = str; q < next_s; q++) {
      size_t off = (size_t)(q - str_base);
      if (off < mark_len) {
        mark[off] = TRUE;
      }
    }
    return TRUE;
  }
  if (*str == '\0') {
    return FALSE;
  }
  gunichar sc = g_utf8_get_char(str);
  if (pc != sc) {
    return FALSE;
  }
  {
    const char *next_s = g_utf8_next_char(str);
    if (!wild_match_fold_mark(g_utf8_next_char(pat), next_s, mark, str_base, mark_len, depth)) {
      return FALSE;
    }
    for (const char *q = str; q < next_s; q++) {
      size_t off = (size_t)(q - str_base);
      if (off < mark_len) {
        mark[off] = TRUE;
      }
    }
    return TRUE;
  }
}

static void mark_display_utf8_char(gboolean *dmark, const char *disp, gsize dlen, const char *p) {
  const char *next = g_utf8_next_char(p);
  for (const char *q = p; q < next; q++) {
    size_t off = (size_t)(q - disp);
    if (off < dlen) {
      dmark[off] = TRUE;
    }
  }
}

static gchar *da_search_filter_markup_plain_substrings(const char *display, const char *filter_utf8) {
  GString *gs = g_string_new(NULL);
  const char *pending = display;
  const char *p = display;

  while (*p) {
    const char *end_match = NULL;
    if (utf8_prefix_ci_match(p, filter_utf8, &end_match)) {
      if (p > pending) {
        gchar *chunk = g_markup_escape_text(pending, (gssize)(p - pending));
        if (chunk != NULL) {
          g_string_append(gs, chunk);
        }
        g_free(chunk);
      }
      gchar *mid = g_markup_escape_text(p, (gssize)(end_match - p));
      g_string_append(gs, "<b><span foreground=\"#1565C0\">");
      if (mid != NULL) {
        g_string_append(gs, mid);
      }
      g_string_append(gs, "</span></b>");
      g_free(mid);
      p = end_match;
      pending = p;
    } else {
      p = g_utf8_next_char(p);
    }
  }
  if (*pending != '\0') {
    gchar *tail = g_markup_escape_text(pending, -1);
    if (tail != NULL) {
      g_string_append(gs, tail);
    }
    g_free(tail);
  }
  gchar *out = g_strdup(gs->str);
  g_string_free(gs, TRUE);
  return out;
}

static gchar *da_search_filter_markup_wildcard(const char *display, const char *filter_utf8) {
  gchar *fold_str = g_utf8_casefold(display, -1);
  if (fold_str == NULL) {
    return g_markup_escape_text(display, -1);
  }

  gsize flen = strlen(fold_str);
  GArray *map = g_array_new(FALSE, FALSE, sizeof(gint));
  const char *fp = fold_str;
  gboolean mapping_ok = TRUE;

  for (const char *p = display; *p; p = g_utf8_next_char(p)) {
    const char *next = g_utf8_next_char(p);
    gint ds = (gint)(p - display);
    gchar *cf = g_utf8_casefold(p, (gssize)(next - p));
    if (cf == NULL) {
      mapping_ok = FALSE;
      break;
    }
    gsize clen = strlen(cf);
    gsize consumed = (gsize)(fp - fold_str);
    if (flen < consumed + clen || strncmp(fp, cf, clen) != 0) {
      mapping_ok = FALSE;
      g_free(cf);
      break;
    }
    for (gsize j = 0; j < clen; j++) {
      gint v = ds;
      g_array_append_val(map, v);
    }
    fp += clen;
    g_free(cf);
  }

  if (!mapping_ok || (size_t)(fp - fold_str) != flen || (gsize)map->len != flen) {
    g_free(fold_str);
    g_array_free(map, TRUE);
    return g_markup_escape_text(display, -1);
  }

  gchar *pat_fold = g_utf8_casefold(filter_utf8, -1);
  if (pat_fold == NULL) {
    g_free(fold_str);
    g_array_free(map, TRUE);
    return g_markup_escape_text(display, -1);
  }

  gboolean *fmark = g_malloc0((flen + 1u) * sizeof(gboolean));
  gboolean ok = wild_match_fold_mark(pat_fold, fold_str, fmark, fold_str, flen, 0);
  g_free(pat_fold);
  g_free(fold_str);

  gsize dlen = strlen(display);
  gboolean *dmark = g_malloc0((dlen + 1u) * sizeof(gboolean));
  if (ok) {
    for (gsize i = 0; i < flen; i++) {
      if (fmark[i]) {
        gint ds = g_array_index(map, gint, i);
        if (ds >= 0 && (gsize)ds < dlen) {
          mark_display_utf8_char(dmark, display, dlen, display + (size_t)ds);
        }
      }
    }
  }

  GString *gs = g_string_new(NULL);
  const char *const disp_end = display + dlen;
  for (const char *p = display; *p != '\0' && p < disp_end;) {
    if (!g_utf8_validate(p, (gssize)(disp_end - p), NULL)) {
      gchar *t = g_markup_escape_text(p, (gssize)(disp_end - p));
      if (t != NULL) {
        g_string_append(gs, t);
      }
      g_free(t);
      break;
    }
    const char *next = g_utf8_next_char(p);
    if (next <= p || next > disp_end) {
      gchar *t = g_markup_escape_text(p, (gssize)(disp_end - p));
      if (t != NULL) {
        g_string_append(gs, t);
      }
      g_free(t);
      break;
    }
    gboolean hl = FALSE;
    for (const char *q = p; q < next; q++) {
      size_t off = (size_t)(q - display);
      if (off < dlen && dmark[off]) {
        hl = TRUE;
        break;
      }
    }
    const char *q = next;
    while (q < disp_end && *q != '\0') {
      const char *qn = g_utf8_next_char(q);
      if (qn <= q || qn > disp_end) {
        break;
      }
      gboolean h2 = FALSE;
      for (const char *r = q; r < qn; r++) {
        size_t off = (size_t)(r - display);
        if (off < dlen && dmark[off]) {
          h2 = TRUE;
          break;
        }
      }
      if (h2 != hl) {
        break;
      }
      q = qn;
    }
    if (q > disp_end) {
      q = disp_end;
    }
    gssize chunk_len = (gssize)(q - p);
    gssize max_span = (gssize)(disp_end - p);
    if (chunk_len > max_span) {
      chunk_len = max_span;
      q = p + (size_t)max_span;
    }
    if (chunk_len <= 0) {
      if (*p == '\0') {
        break;
      }
      p = g_utf8_next_char(p);
      continue;
    }
    gchar *escaped = g_markup_escape_text(p, chunk_len);
    gchar *chunk = escaped != NULL ? g_strdup(escaped) : NULL;
    g_free(escaped);
    if (hl) {
      g_string_append(gs, "<b><span foreground=\"#1565C0\">");
      if (chunk != NULL) {
        g_string_append(gs, chunk);
      }
      g_string_append(gs, "</span></b>");
    } else {
      if (chunk != NULL) {
        g_string_append(gs, chunk);
      }
    }
    g_free(chunk);
    p = q;
  }

  gchar *out = g_strdup(gs->str);
  g_string_free(gs, TRUE);
  g_free(fmark);
  g_free(dmark);
  g_array_free(map, TRUE);
  return out;
}

gchar *da_search_filter_markup(const gchar *display_utf8, const gchar *filter_utf8) {
  if (filter_utf8 == NULL || filter_utf8[0] == '\0') {
    return NULL;
  }
  const char *display = display_utf8 != NULL ? display_utf8 : "";
  if (!g_utf8_validate(display, -1, NULL)) {
    return g_markup_escape_text(display, -1);
  }
  if (da_filter_has_wildcard(filter_utf8)) {
    return da_search_filter_markup_wildcard(display, filter_utf8);
  }
  return da_search_filter_markup_plain_substrings(display, filter_utf8);
}
