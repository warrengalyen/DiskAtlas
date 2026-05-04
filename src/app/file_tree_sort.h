#ifndef FILE_TREE_SORT_H
#define FILE_TREE_SORT_H

#include <gtk/gtk.h>

#include "app_state.h"

void da_file_tree_install_sorting(GtkTreeView *tv, GtkTreeStore *store, AppState *app);

#endif
