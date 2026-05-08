#ifndef FORMAT_TEXT_H
#define FORMAT_TEXT_H

#include <glib.h>
#include <stddef.h>
#include <stdint.h>

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
 * Pango markup for file-view search highlighting: case-insensitive occurrences of
 * filter_utf8 are bold and blue; other text is escaped. Returns NULL when filter_utf8 is
 * empty or contains * or ? (caller should show plain display_utf8 instead).
 */
gchar *da_search_filter_markup(const gchar *display_utf8, const gchar *filter_utf8);

#endif  /* FORMAT_TEXT_H */
