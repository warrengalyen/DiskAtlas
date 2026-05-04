#ifndef SCAN_CONTROLLER_H
#define SCAN_CONTROLLER_H

#include "app_state.h"

void scan_controller_attach(AppState *app);
void scan_controller_detach(AppState *app);
void scan_controller_refresh_volume_labels(AppState *app);
void scan_controller_sync_display_max_combo(AppState *app);

/** Restores the bottom status line to the normal scan summary (after treemap hover ends). */
void scan_controller_restore_scan_status(AppState *app);

#endif
