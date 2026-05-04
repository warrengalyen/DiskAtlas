#ifndef FORMAT_TEXT_H
#define FORMAT_TEXT_H

#include <stddef.h>
#include <stdint.h>

void da_format_bytes(uint64_t n, char *dst, size_t dstsz);
void da_format_bytes_with_pct(uint64_t bytes, uint64_t vol_total, char *dst, size_t dstsz);
void da_format_pct_of_volume(uint64_t file_bytes, uint64_t vol_total, char *dst, size_t dstsz);
/** One decimal and a space before the percent sign (e.g. "34.0 %"); em dash if vol_total == 0. */
void da_format_pct_progress_label(uint64_t file_bytes, uint64_t vol_total, char *dst, size_t dstsz);
void da_format_mtime_local(uint64_t unix_ns, char *dst, size_t dstsz);
void da_format_win32_attr_letters(uint32_t wa, char *dst, size_t dstsz);

#endif  /* FORMAT_TEXT_H */
