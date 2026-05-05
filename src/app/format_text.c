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

void da_format_bytes(uint64_t n, char *dst, size_t dstsz) {
  if (!dst || dstsz == 0) {
    return;
  }
  if (n < 1024ull) {
    snprintf(dst, dstsz, "%" PRIu64 " B", n);
    return;
  }
  if (n < 1024ull * 1024ull) {
    snprintf(dst, dstsz, "%.2f KiB", (double)n / 1024.0);
    return;
  }
  if (n < 1024ull * 1024ull * 1024ull) {
    snprintf(dst, dstsz, "%.2f MiB", (double)n / (1024.0 * 1024.0));
    return;
  }
  if (n < 1024ull * 1024ull * 1024ull * 1024ull) {
    snprintf(dst, dstsz, "%.2f GiB", (double)n / (1024.0 * 1024.0 * 1024.0));
    return;
  }
  snprintf(dst, dstsz, "%.2f TiB",
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
    snprintf(dst, dstsz, "—");
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
    snprintf(dst, dstsz, "—");
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
    snprintf(dst, dstsz, "—");
    return;
  }
  GDateTime *utc =
      g_date_time_new_from_unix_utc((gint64)(unix_ns / 1000000000ull));
  if (utc == NULL) {
    snprintf(dst, dstsz, "—");
    return;
  }
  GDateTime *loc = g_date_time_to_local(utc);
  gchar *s = g_date_time_format(loc, "%Y-%m-%d %H:%M:%S");
  if (s != NULL) {
    g_strlcpy(dst, s, dstsz);
    g_free(s);
  } else {
    snprintf(dst, dstsz, "—");
  }
  g_date_time_unref(loc);
  g_date_time_unref(utc);
}

void da_format_win32_attr_letters(uint32_t wa, char *dst, size_t dstsz) {
  if (!dst || dstsz == 0) {
    return;
  }
  if (wa == 0) {
    snprintf(dst, dstsz, "—");
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
#endif
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
