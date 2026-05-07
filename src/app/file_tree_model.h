#ifndef FILE_TREE_MODEL_H
#define FILE_TREE_MODEL_H

#include <glib.h>
#include <stdint.h>

#include "app_state.h"

/** Lazy-expand placeholder row LP (not a real scan node index). */
#define DA_TREE_LP_PLACEHOLDER ((gint64)INT64_MIN)

gboolean da_utf8_basename_matches_filter(const char *path_utf8, const char *filter_utf8);

gboolean da_duplicates_only(const AppState *app);
gboolean da_view_uses_filtered_pool(const AppState *app);

/** TRUE when the file list should include directory entries (checkbox default off). */
gboolean da_show_folders_in_file_list(const AppState *app);
gboolean da_node_shown_in_file_view(const AppState *app, const scan_results_view_t *v, size_t nid);

size_t da_source_pool_count(const AppState *app);
size_t da_source_count(const AppState *app);
size_t da_source_at(const AppState *app, size_t i);

/** Maps LP column to scan node index; returns TRUE with *out_nid == SIZE_MAX for placeholder rows. */
gboolean da_tree_lp_to_scan_nid(AppState *app, gint64 lp, size_t *out_nid);

#endif  /* FILE_TREE_MODEL_H */
