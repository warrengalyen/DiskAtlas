#ifndef UI_WINDOW_H
#define UI_WINDOW_H

#include "app_state.h"

void da_ui_build(AppState *app);

/**
 * Enable File → Export CSV / Copy size info when a completed scan has node data; enable Explore
 * Folder, Terminal/Cmd Here, and Copy Path when that holds and the active view has an explicit
 * selection (file list, treemap, or Tree View).
 */
void da_ui_sync_file_menu_export_csv(AppState *app);

#endif
