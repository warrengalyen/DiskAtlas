#include <inttypes.h>
#include <stdio.h>
#include <string.h>

#include <glib.h>

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
