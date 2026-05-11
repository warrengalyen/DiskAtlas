#ifndef SCAN_CONTROLLER_H
#define SCAN_CONTROLLER_H

#include "app_state.h"

void scan_controller_attach(AppState *app);
void scan_controller_detach(AppState *app);

/** File-view search: editable combo, history rows, `<clear>` row; call after `app->search` is loaded from UI. */
void da_file_view_search_combo_init(AppState *app);

/** Start a new scan, or cancel the current one if still running (same as the Scan toolbar button). */
void scan_controller_request_scan(AppState *app);
void scan_controller_refresh_volume_labels(AppState *app);
/** After changing size decimal-place preference: refresh all on-screen byte strings. */
void scan_controller_refresh_size_display_format(AppState *app);
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

/** Copy size (column 1, right-aligned) and full path (column 2) for current selection or entire scan/list. */
void scan_controller_copy_scan_paths_sizes_to_clipboard(AppState *app);

/** TRUE when Export CSV would be enabled and the active view has at least one explicit selected path. */
gboolean scan_controller_file_menu_selection_commands_sensitive(AppState *app);

/** Open each distinct parent folder of the current selection in the system file manager. */
void scan_controller_explore_selected_folders(AppState *app);

/** Open an interactive shell in each distinct folder of the current selection (platform default terminal/cmd). */
void scan_controller_open_terminal_here(AppState *app);

/** Copy full UTF-8 paths of selected items to the clipboard, one per line. */
void scan_controller_copy_selected_paths_to_clipboard(AppState *app);

/** Copy selected file/folder paths to the system clipboard (shell copy). */
void scan_controller_copy_files(AppState *app);

/** Cut (move) selected file/folder paths to the system clipboard. */
void scan_controller_cut_files(AppState *app);

/** Move selected file/folder(s) to the system trash / recycle bin and mark
 *  them visually as deleted in both tree views. */
void scan_controller_delete_to_trash(AppState *app);

/** Permanently delete selected file/folder(s) after a confirmation dialog.
 *  Marks surviving items visually as deleted in both tree views. */
void scan_controller_delete_permanent(AppState *app);

/**
 * Reclassify all currently loaded scan nodes using app->mime_db and queue a treemap redraw.
 * No-op when no scan is loaded or mime_db is NULL.
 */
void scan_controller_reclassify_mime(AppState *app);

#if defined(G_OS_WIN32)
/** After validations in the UI: optionally scan @a volume_root_utf8, then copy $MFT to @a dest_path_utf8. */
void scan_controller_begin_mft_dump_flow(AppState *app, const gchar *volume_root_utf8,
                                         const gchar *dest_path_utf8, gboolean need_scan);
#endif

#endif  /* SCAN_CONTROLLER_H */
