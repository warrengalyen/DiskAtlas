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

#endif
