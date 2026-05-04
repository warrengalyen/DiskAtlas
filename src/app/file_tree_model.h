#ifndef FILE_TREE_MODEL_H
#define FILE_TREE_MODEL_H

#include <glib.h>

#include "app_state.h"

GtkTreeStore *da_tree_store_new(void);
void da_tree_clear(AppState *app);
/** Returns TRUE if more root rows remain (start tree insert timer). */
gboolean da_tree_begin_root_insert(AppState *app);
gboolean da_tree_insert_roots_chunk(AppState *app);
void da_tree_on_row_expanded(GtkTreeView *tv, GtkTreeIter *iter, GtkTreePath *path,
                             gpointer user_data);

gboolean da_utf8_basename_matches_filter(const char *path_utf8, const char *filter_utf8);

size_t da_source_pool_count(const AppState *app);
size_t da_source_count(const AppState *app);
size_t da_source_at(const AppState *app, size_t i);

#endif
