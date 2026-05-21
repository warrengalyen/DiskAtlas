#ifndef FILE_TREE_MODEL_H
#define FILE_TREE_MODEL_H

#include <glib.h>
#include <stdint.h>

#include "app_state.h"

/** Lazy-expand placeholder row LP (not a real scan node index). */
#define DA_TREE_LP_PLACEHOLDER ((gint64)INT64_MIN)

gboolean da_utf8_basename_matches_filter(const char *path_utf8, const char *filter_utf8);

/** File View filter: basename or full path per toolbar radios; supports * and ? (UTF-8 aware). */
gboolean da_utf8_file_view_filter_matches(const AppState *app, const char *path_utf8);

gboolean da_duplicates_only(const AppState *app);
gboolean da_view_uses_filtered_pool(const AppState *app);

/** TRUE when the file list should include directory entries (checkbox default off). */
gboolean da_show_folders_in_file_list(const AppState *app);

/** TRUE when Options / Settings allow showing hidden and recycle-bin paths in tree and file lists. */
gboolean da_view_hidden_files(const AppState *app);

/** TRUE when @a path_utf8 is under $Recycle.Bin (case-insensitive). */
gboolean da_path_is_under_recycle_bin_ci(const char *path_utf8);

/** TRUE when @a path_utf8 is under $Recycle.Bin or basename is a recycle $I/$R internal name. */
gboolean da_path_is_recycle_internal_name_ci(const char *path_utf8);

/** TRUE when a scan node is hidden on disk (Win32 HIDDEN or under $Recycle.Bin). */
gboolean da_node_is_hidden(const file_node_t *node);

/** TRUE when @a path should render as hidden (recycle path, Win32 HIDDEN bit, or @a scan_node). */
gboolean da_path_is_hidden_for_display(const char *path_utf8, uint32_t win32_attributes,
                                     const file_node_t *scan_node);

gboolean da_node_shown_in_file_view(const AppState *app, const scan_results_view_t *v, size_t nid);

size_t da_source_pool_count(const AppState *app);
size_t da_source_count(const AppState *app);
size_t da_source_at(const AppState *app, size_t i);

/** Maps LP column to scan node index; returns TRUE with *out_nid == SIZE_MAX for placeholder rows. */
gboolean da_tree_lp_to_scan_nid(AppState *app, gint64 lp, size_t *out_nid);

#endif  /* FILE_TREE_MODEL_H */
