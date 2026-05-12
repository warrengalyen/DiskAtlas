#ifndef FORMAT_TEXT_H
#define FORMAT_TEXT_H

#include <glib.h>
#include <stddef.h>
#include <stdint.h>

/** How `da_format_bytes` chooses units; persisted under `[interface]` `size_display_format` in diskatlas.ini. */
typedef enum {
  DA_SIZE_DISPLAY_DYNAMIC = 0,
  DA_SIZE_DISPLAY_BYTES = 1,
  DA_SIZE_DISPLAY_KB = 2,
  DA_SIZE_DISPLAY_MB = 3,
  DA_SIZE_DISPLAY_GB = 4,
  DA_SIZE_DISPLAY_TB = 5,
} DaSizeDisplayFormat;

/** Clamp to 0–4; affects all `da_format_bytes` / `da_format_bytes_with_pct` output for fractional units. */
void da_format_bytes_set_decimal_places(int places);
int da_format_bytes_get_decimal_places(void);

/** Clamp to a `DaSizeDisplayFormat` value; drives `da_format_bytes` / `da_format_bytes_with_pct`. */
void da_format_bytes_set_display_format(int format);
int da_format_bytes_get_display_format(void);

void da_format_bytes(uint64_t n, char *dst, size_t dstsz);
void da_format_bytes_with_pct(uint64_t bytes, uint64_t vol_total, char *dst, size_t dstsz);
void da_format_pct_of_volume(uint64_t file_bytes, uint64_t vol_total, char *dst, size_t dstsz);
/** One decimal and a space before the percent sign (e.g. "34.0 %"); em dash if vol_total == 0. */
void da_format_pct_progress_label(uint64_t file_bytes, uint64_t vol_total, char *dst, size_t dstsz);
void da_format_mtime_local(uint64_t unix_ns, char *dst, size_t dstsz);
void da_format_win32_attr_letters(uint32_t wa, char *dst, size_t dstsz);
/** Unsigned integer using LC_NUMERIC grouping and thousands_sep (see localeconv(3)). */
void da_format_uint64_locale(uint64_t n, char *dst, size_t dstsz);

/**
 * Pango markup for file-view search highlighting: case-insensitive matches are bold and
 * blue (substring search, or glob-aligned spans when filter uses * / ?). Returns NULL
 * only when filter_utf8 is empty (caller should show plain display_utf8).
 */
gchar *da_search_filter_markup(const gchar *display_utf8, const gchar *filter_utf8);

#endif  /* FORMAT_TEXT_H */
