#ifndef CSV_EXPORT_H
#define CSV_EXPORT_H

#include <stddef.h>

#include <gtk/gtk.h>

#include "app_state.h"

/**
 * Export the current completed scan to CSV (RFC4180-style, UTF-8).
 * Columns: File Name (full path), Size, Allocated, Modified (same local format as the file list:
 * `YYYY-MM-DD HH:MM:SS`), Attributes (decimal win32 flags), Files / Folders (subtree counts for directories),
 * then volume totals from da_volume_space_for_path on app->scan_root_utf8: DRIVECAPACITY, FREESPACE, USEDSPACE,
 * and optionally RESERVEDSPACE.
 */
int da_export_scan_csv(AppState *app, const char *utf8_path, gboolean include_reserved_space_column, char *errbuf,
                       size_t errlen);

/**
 * Export the File Types list (same logical columns as the tree view except the color swatch) to CSV
 * (RFC4180-style, UTF-8). Extension and File Type are quoted when needed; Percent is an integer 0–100
 * (no percent sign); Size and Allocated are byte counts; Files is a plain integer (no grouping).
 * Requires a completed scan with node data; reads the current rows from `app->file_type_tree` in
 * model order (sorted order when the list is sorted).
 */
int da_export_file_types_csv(AppState *app, const char *utf8_path, char *errbuf, size_t errlen);

#endif  /* CSV_EXPORT_H */
