#ifndef UI_WINDOW_H
#define UI_WINDOW_H

#include "app_state.h"

void da_ui_build(AppState *app);

/** Enable File → Export CSV only when a completed scan has exportable node data. */
/** Enable File → Export CSV and Copy to clipboard when a completed scan has node data. */
void da_ui_sync_file_menu_export_csv(AppState *app);

#endif
