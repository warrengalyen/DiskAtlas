#ifndef SCAN_CONTROLLER_H
#define SCAN_CONTROLLER_H

#include "app_state.h"

void scan_controller_attach(AppState *app);
void scan_controller_detach(AppState *app);
void scan_controller_refresh_volume_labels(AppState *app);
void scan_controller_sync_display_max_combo(AppState *app);

/** Updates the three-part status bar when the File View tab is active (selection, hover path, list totals). */
void scan_controller_sync_file_view_status(AppState *app);

#endif
