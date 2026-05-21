#include <ctype.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include <glib.h>
#include <gtk/gtk.h>

#include "diskatlas.h"
#include "diskatlas_wildcard.h"
#include "file_tree_model.h"
#include "format_text.h"

gboolean da_duplicates_only(const AppState *app) {
  if (app == NULL || app->duplicates_only_check == NULL) {
    return FALSE;
  }
  return gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(app->duplicates_only_check));
}

gboolean da_view_uses_filtered_pool(const AppState *app) {
  if (app == NULL) {
    return FALSE;
  }
  return app->filter_active || da_duplicates_only(app);
}

gboolean da_show_folders_in_file_list(const AppState *app) {
  if (app == NULL || app->show_folders_check == NULL) {
    return FALSE;
  }
  return gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(app->show_folders_check));
}

gboolean da_node_shown_in_file_view(const AppState *app, const scan_results_view_t *v, size_t nid) {
  if (v == NULL || v->nodes == NULL || nid >= v->count) {
    return FALSE;
  }
  uint32_t kind = v->nodes[nid].attributes & DISKATLAS_NODE_KIND_MASK;
  if (kind == DISKATLAS_NODE_KIND_DIR && !da_show_folders_in_file_list(app)) {
    return FALSE;
  }
  return TRUE;
}

gboolean da_utf8_basename_matches_filter(const char *path_utf8, const char *filter_utf8) {
  return diskatlas_utf8_matches_filter(path_utf8, filter_utf8, TRUE) ? TRUE : FALSE;
}

gboolean da_utf8_file_view_filter_matches(const AppState *app, const char *path_utf8) {
  if (app == NULL) {
    return FALSE;
  }
  const char *path = path_utf8 != NULL ? path_utf8 : "";
  gboolean entire = (app->match_entire_path_radio != NULL &&
                     gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(app->match_entire_path_radio)));
  return diskatlas_utf8_matches_filter(path, app->filter_text, !entire) ? TRUE : FALSE;
}

size_t da_source_pool_count(const AppState *app) {
  if (app == NULL) {
    return 0;
  }
  return da_view_uses_filtered_pool(app) ? app->filtered_count : app->master_count;
}

size_t da_source_count(const AppState *app) {
  if (app == NULL) {
    return 0;
  }
  size_t pool = da_source_pool_count(app);
  size_t cap = app->display_max_entries;
  if (cap == 0) {
    return pool;
  }
  return pool < cap ? pool : cap;
}

size_t da_source_at(const AppState *app, size_t i) {
  if (da_view_uses_filtered_pool(app)) {
    return app->filtered_indices[i];
  }
  return app->master_indices[i];
}

gboolean da_tree_lp_to_scan_nid(AppState *app, gint64 lp, size_t *out_nid) {
  if (out_nid == NULL) {
    return FALSE;
  }
  if (lp == DA_TREE_LP_PLACEHOLDER) {
    *out_nid = SIZE_MAX;
    return TRUE;
  }
  if (lp > 0) {
    *out_nid = (size_t)(lp - 1);
    return TRUE;
  }
  /* Negative LP (dup-group parent row) is no longer used with FlatListModel,
   * but handle it defensively for any residual code paths. */
  if (lp < 0 && app != NULL && app->scan != NULL) {
    uint32_t gid = (uint32_t)(-lp);
    size_t nmem = 0;
    const size_t *mp = diskatlas_dup_group_members(app->scan, gid, &nmem);
    scan_results_view_t v = scan_get_results(app->scan);
    if (mp != NULL && nmem > 0 && mp[0] < v.count) {
      *out_nid = mp[0];
      return TRUE;
    }
  }
  return FALSE;
}
