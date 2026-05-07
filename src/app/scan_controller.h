#ifndef SCAN_CONTROLLER_H
#define SCAN_CONTROLLER_H

#include "app_state.h"

void scan_controller_attach(AppState *app);
void scan_controller_detach(AppState *app);

/** Start a new scan, or cancel the current one if still running (same as the Scan toolbar button). */
void scan_controller_request_scan(AppState *app);
void scan_controller_refresh_volume_labels(AppState *app);
void scan_controller_sync_display_max_combo(AppState *app);

/** Updates the three-part status bar when the File View tab is active (selection, hover path, list totals). */
void scan_controller_sync_file_view_status(AppState *app);

/** Call after `scan_root_utf8` changes (same side effects as former scan directory picker). */
void scan_controller_notify_scan_root_changed(AppState *app);

/**
 * Replace current scan with an imported result; refreshes list like a finished scan.
 * @param raw_mft_snapshot TRUE when the source is a raw $MFT dump (UI labels); FALSE for CSV.
 */
void scan_controller_apply_imported_scan(AppState *app, scan_result_t *new_scan, const char *snapshot_path_utf8,
                                           gboolean snapshot_layout, gboolean raw_mft_snapshot);

/** Duplicate-clustering options aligned with the toolbar (for MFT dump import). */
void scan_controller_fill_scan_options_for_import(AppState *app, scan_options_t *out);

#if defined(G_OS_WIN32)
/** After validations in the UI: optionally scan @a volume_root_utf8, then copy $MFT to @a dest_path_utf8. */
void scan_controller_begin_mft_dump_flow(AppState *app, const gchar *volume_root_utf8,
                                         const gchar *dest_path_utf8, gboolean need_scan);
#endif

#endif  /* SCAN_CONTROLLER_H */
