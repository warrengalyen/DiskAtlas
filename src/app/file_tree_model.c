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

#if defined(G_OS_WIN32)
#include <windows.h>
#endif

/** TRUE when @a seg (NUL-terminated at @a seg_len) is "$Recycle.Bin" (any common casing). */
static gboolean da_path_seg_is_recycle_bin_ci(const char *seg, size_t seg_len) {
  return seg_len == 13 && g_ascii_strncasecmp(seg, "$Recycle.Bin", 13) == 0;
}

gboolean da_path_is_under_recycle_bin_ci(const char *path_utf8) {
  if (path_utf8 == NULL || path_utf8[0] == '\0') {
    return FALSE;
  }
  /* First component may be "$RECYCLE.BIN" with no leading separator (e.g. "D:\$RECYCLE.BIN\..."). */
  {
    const char *end = path_utf8;
    while (*end != '\0' && *end != '\\' && *end != '/') {
      end++;
    }
    if (da_path_seg_is_recycle_bin_ci(path_utf8, (size_t)(end - path_utf8))) {
      return TRUE;
    }
  }
  for (const char *p = path_utf8; *p != '\0'; p++) {
    if ((*p == '\\' || *p == '/') && p[1] != '\0') {
      const char *seg = p + 1;
      const char *end = seg;
      while (*end != '\0' && *end != '\\' && *end != '/') {
        end++;
      }
      if (da_path_seg_is_recycle_bin_ci(seg, (size_t)(end - seg))) {
        return TRUE;
      }
    }
  }
  {
    gchar *lower = g_utf8_strdown(path_utf8, -1);
    if (lower != NULL) {
      const char *needle = "$recycle.bin";
      const size_t nlen = 13;
      for (const char *hit = lower; (hit = strstr(hit, needle)) != NULL; hit++) {
        const char before = (hit == lower) ? '/' : hit[-1];
        const char after = hit[nlen];
        if ((hit == lower || before == '\\' || before == '/') &&
            (after == '\0' || after == '\\' || after == '/')) {
          g_free(lower);
          return TRUE;
        }
        if (hit[0] != '\0') {
          hit++;
        }
      }
      g_free(lower);
    }
  }
  return FALSE;
}

gboolean da_path_is_recycle_internal_name_ci(const char *path_utf8) {
  if (path_utf8 == NULL || path_utf8[0] == '\0') {
    return FALSE;
  }
  if (da_path_is_under_recycle_bin_ci(path_utf8)) {
    return TRUE;
  }
  const char *base = path_utf8;
  for (const char *p = path_utf8; *p != '\0'; p++) {
    if (*p == '\\' || *p == '/') {
      base = p + 1;
    }
  }
  if (base[0] == '$' && (base[1] == 'I' || base[1] == 'i' || base[1] == 'R' || base[1] == 'r')) {
    return TRUE;
  }
  return FALSE;
}

gboolean da_node_is_hidden(const file_node_t *node) {
  if (node == NULL) {
    return FALSE;
  }
  return da_path_is_hidden_for_display(node->path, node->win32_attributes, node);
}

gboolean da_path_is_hidden_for_display(const char *path_utf8, uint32_t win32_attributes,
                                         const file_node_t *scan_node) {
  if (scan_node != NULL) {
    if (da_path_is_recycle_internal_name_ci(scan_node->path)) {
      return TRUE;
    }
#if defined(G_OS_WIN32)
    if ((scan_node->win32_attributes & FILE_ATTRIBUTE_HIDDEN) != 0) {
      return TRUE;
    }
#endif
  }
  if (path_utf8 != NULL && da_path_is_recycle_internal_name_ci(path_utf8)) {
    return TRUE;
  }
#if defined(G_OS_WIN32)
  if ((win32_attributes & FILE_ATTRIBUTE_HIDDEN) != 0) {
    return TRUE;
  }
#endif
#if !defined(G_OS_WIN32)
  if (path_utf8 != NULL) {
    const char *base = path_utf8;
    for (const char *p = path_utf8; *p != '\0'; p++) {
      if (*p == '/') {
        base = p + 1;
      }
    }
    if (base[0] == '.') {
      return TRUE;
    }
  }
#endif
  return FALSE;
}

gboolean da_view_hidden_files(const AppState *app) {
  if (app == NULL) {
    return TRUE;
  }
  return app->interface_view_hidden_files;
}

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
  const file_node_t *node = &v->nodes[nid];
  uint32_t kind = node->attributes & DISKATLAS_NODE_KIND_MASK;
  if (kind == DISKATLAS_NODE_KIND_DIR && !da_show_folders_in_file_list(app)) {
    return FALSE;
  }
  if (!da_view_hidden_files(app) && da_node_is_hidden(node)) {
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
