#ifndef UI_WINDOW_H
#define UI_WINDOW_H

#include "app_state.h"

void da_ui_build(AppState *app);

/**
 * Zebra + Allocated column fill: sets renderer background from `interface_alternate_row_colors`
 * and row band (GtkTreeView fixed-height rows). Call from GtkTreeView cell data functions.
 * @param column_is_allocated TRUE for Allocated columns (always #EFEFEF when not selected).
 */
void da_tree_view_apply_zebra_cell(GtkTreeViewColumn *col, GtkCellRenderer *cell, GtkTreeModel *model,
                                   GtkTreeIter *iter, AppState *app, gboolean column_is_allocated);

/**
 * Enable File → Export CSV / Copy size info when a completed scan has node data; enable Explore
 * Folder, Terminal/Cmd Here, and Copy Path when that holds and the active view has an explicit
 * selection (file list, treemap, or Tree View).
 */
void da_ui_sync_file_menu_export_csv(AppState *app);

#endif
