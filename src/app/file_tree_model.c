#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include <glib.h>
#include <gtk/gtk.h>

#include "diskatlas.h"
#include "file_tree_model.h"
#include "format_text.h"

/** Lazy-expand marker row under duplicate-group parents so GtkTreeView shows an expander (GtkTreeStore
 * has no Win32-style “has children” flag without actual child rows). */
#define DA_TREE_LP_CHILD_PLACEHOLDER DA_TREE_LP_PLACEHOLDER

static const char *utf8_basename_ptr(const char *path) {
  const char *base = path ? path : "";
  for (const char *q = base; *q; q++) {
    if (*q == '/' || *q == '\\') {
      base = q + 1;
    }
  }
  return base;
}

static gboolean filter_has_wildcard(const char *filter) {
  if (!filter) {
    return FALSE;
  }
  for (const char *s = filter; *s; s = g_utf8_next_char(s)) {
    gunichar c = g_utf8_get_char(s);
    if (c == '*' || c == '?') {
      return TRUE;
    }
  }
  return FALSE;
}

static gboolean utf8_contains_ci_folded(const char *hay_fold, const char *needle_fold) {
  if (!needle_fold || !needle_fold[0]) {
    return TRUE;
  }
  if (!hay_fold) {
    hay_fold = "";
  }
  return strstr(hay_fold, needle_fold) != NULL;
}

static gboolean wild_match_ci_folded_recursive(const char *pat, const char *str, int depth) {
  if (depth++ > 10000) {
    return FALSE;
  }
  if (*pat == '\0') {
    return *str == '\0';
  }
  if (*pat == '*') {
    if (pat[1] == '\0') {
      return TRUE;
    }
    while (*str != '\0') {
      if (wild_match_ci_folded_recursive(pat + 1, str, depth)) {
        return TRUE;
      }
      str++;
    }
    return wild_match_ci_folded_recursive(pat + 1, str, depth);
  }
  if (*pat == '?') {
    return (*str != '\0') && wild_match_ci_folded_recursive(pat + 1, str + 1, depth);
  }
  if (*pat != *str) {
    return FALSE;
  }
  return wild_match_ci_folded_recursive(pat + 1, str + 1, depth);
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
  uint32_t kind = v->nodes[nid].attributes & DISKATLAS_NODE_KIND_MASK;
  if (kind == DISKATLAS_NODE_KIND_DIR && !da_show_folders_in_file_list(app)) {
    return FALSE;
  }
  return TRUE;
}

gboolean da_utf8_basename_matches_filter(const char *path_utf8, const char *filter_utf8) {
  if (!filter_utf8 || !filter_utf8[0]) {
    return TRUE;
  }
  const char *bn = utf8_basename_ptr(path_utf8);
  gchar *bn_fold = g_utf8_casefold(bn, -1);
  gchar *fi_fold = g_utf8_casefold(filter_utf8, -1);
  gboolean ok;
  if (filter_has_wildcard(filter_utf8)) {
    ok = wild_match_ci_folded_recursive(fi_fold, bn_fold, 0);
  } else {
    ok = utf8_contains_ci_folded(bn_fold, fi_fold);
  }
  g_free(bn_fold);
  g_free(fi_fold);
  return ok;
}

GtkTreeStore *da_tree_store_new(void) {
  return gtk_tree_store_new(DA_N_MODEL_COLS, G_TYPE_STRING, G_TYPE_STRING, G_TYPE_STRING,
                            G_TYPE_STRING, G_TYPE_STRING, G_TYPE_STRING, G_TYPE_STRING, G_TYPE_STRING,
                            G_TYPE_STRING, G_TYPE_INT, G_TYPE_INT64);
}

static uint64_t dup_group_total_size(scan_result_t *scan, const scan_results_view_t *v,
                                     uint32_t gid) {
  size_t nmem = 0;
  const size_t *mp = diskatlas_dup_group_members(scan, gid, &nmem);
  uint64_t sum = 0;
  if (mp == NULL || v == NULL || v->nodes == NULL) {
    return 0;
  }
  for (size_t i = 0; i < nmem; i++) {
    if (mp[i] < v->count) {
      sum += v->nodes[mp[i]].size_bytes;
    }
  }
  return sum;
}

static void fill_row_strings(AppState *app, size_t nid, char col[DA_COL_COUNT][512], gint *pct_bar) {
  for (int i = 0; i < DA_COL_COUNT; i++) {
    col[i][0] = '\0';
  }
  if (pct_bar != NULL) {
    *pct_bar = -1;
  }
  scan_results_view_t v = scan_get_results(app->scan);
  if (app->scan == NULL || v.nodes == NULL || nid >= v.count) {
    return;
  }
  const file_node_t *n = &v.nodes[nid];
  const char *bn = utf8_basename_ptr(n->path);
  g_strlcpy(col[0], bn, sizeof(col[0]));
  g_strlcpy(col[1], n->path, sizeof(col[1]));
  da_format_pct_progress_label(n->size_bytes, app->volume_pct_denominator_bytes, col[2], sizeof(col[2]));
  if (pct_bar != NULL && app->volume_pct_denominator_bytes > 0u) {
    double p = 100.0 * (double)n->size_bytes / (double)app->volume_pct_denominator_bytes;
    if (p > 100.0) {
      p = 100.0;
    }
    *pct_bar = (gint)(p + 0.5);
  }
  da_format_bytes(n->size_bytes, col[3], sizeof(col[3]));
  da_format_bytes(n->allocated_bytes, col[4], sizeof(col[4]));
  da_format_mtime_local(n->mtime_unix_ns, col[5], sizeof(col[5]));

  uint32_t gid = n->duplicate_group_id;
  if (gid != DISKATLAS_DUPLICATE_GROUP_NONE) {
    size_t mc = diskatlas_dup_group_member_count(app->scan, gid);
    size_t peers = mc > 0 ? mc - 1u : 0u;
    snprintf(col[6], sizeof(col[6]), "%zu", peers);
    uint64_t dsum = dup_group_total_size(app->scan, &v, gid);
    da_format_bytes(dsum, col[7], sizeof(col[7]));
  } else {
    g_strlcpy(col[6], "—", sizeof(col[6]));
    g_strlcpy(col[7], "—", sizeof(col[7]));
  }
  da_format_win32_attr_letters(n->win32_attributes, col[8], sizeof(col[8]));
}

void da_tree_clear(AppState *app) {
  if (app == NULL || app->store == NULL) {
    return;
  }
  gtk_tree_store_clear(app->store);
  app->tree_insert_pos = 0;
  g_free(app->dup_group_seen);
  app->dup_group_seen = NULL;
  app->dup_group_seen_cap = 0;
}

static int ensure_dup_group_seen(AppState *app, uint32_t max_gid) {
  size_t need = (size_t)max_gid + 1u;
  if (need == 0) {
    need = 16;
  }
  if (app->dup_group_seen != NULL && app->dup_group_seen_cap >= need) {
    memset(app->dup_group_seen, 0, app->dup_group_seen_cap);
    return 0;
  }
  guint8 *nb = (guint8 *)realloc(app->dup_group_seen, need);
  if (nb == NULL) {
    return -1;
  }
  app->dup_group_seen = nb;
  app->dup_group_seen_cap = need;
  memset(app->dup_group_seen, 0, need);
  return 0;
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

gboolean da_tree_begin_root_insert(AppState *app) {
  if (app == NULL || app->store == NULL || app->scan == NULL) {
    return FALSE;
  }
  da_tree_clear(app);
  if (app->master_count == 0) {
    return FALSE;
  }
  /* Flatten roots only when filtering by name; "Duplicates only" still uses grouped dup rows. */
  if (!app->filter_active) {
    uint32_t mg = diskatlas_dup_max_group_id(app->scan);
    if (ensure_dup_group_seen(app, mg) != 0) {
      GtkWidget *d = gtk_message_dialog_new(
          GTK_WINDOW(app->window), GTK_DIALOG_MODAL, GTK_MESSAGE_WARNING, GTK_BUTTONS_OK,
          "Could not allocate duplicate-group state for the tree.");
      gtk_dialog_run(GTK_DIALOG(d));
      gtk_widget_destroy(d);
      return FALSE;
    }
  }
  app->tree_insert_pos = 0;
  return da_tree_insert_roots_chunk(app);
}

gboolean da_tree_insert_roots_chunk(AppState *app) {
  GtkTreeStore *store = app->store;
  scan_results_view_t v = scan_get_results(app->scan);
  if (store == NULL || v.nodes == NULL || app->scan == NULL) {
    return FALSE;
  }

  size_t total = da_source_count(app);
  int batch = 0;

  while (app->tree_insert_pos < total && batch < DA_TREEINSERT_BATCH) {
    size_t nid = da_source_at(app, app->tree_insert_pos++);
    if (nid >= v.count) {
      batch++;
      continue;
    }
    const file_node_t *n = &v.nodes[nid];
    batch++;

    GtkTreeIter iter;
    char col[DA_COL_COUNT][512];
    gint pct = -1;
    gint64 lp;

    if (app->filter_active || n->duplicate_group_id == DISKATLAS_DUPLICATE_GROUP_NONE) {
      fill_row_strings(app, nid, col, &pct);
      lp = (gint64)(nid + 1u);
      gtk_tree_store_insert_with_values(
          store, &iter, NULL, -1, 0, col[0], 1, col[1], 2, col[2], 3, col[3], 4, col[4], 5,
          col[5], 6, col[6], 7, col[7], 8, col[8], DA_COL_PCT, pct, DA_COL_LP, lp, -1);
      continue;
    }

    uint32_t gid = n->duplicate_group_id;
    if (gid >= app->dup_group_seen_cap) {
      fill_row_strings(app, nid, col, &pct);
      lp = (gint64)(nid + 1u);
      gtk_tree_store_insert_with_values(
          store, &iter, NULL, -1, 0, col[0], 1, col[1], 2, col[2], 3, col[3], 4, col[4], 5,
          col[5], 6, col[6], 7, col[7], 8, col[8], DA_COL_PCT, pct, DA_COL_LP, lp, -1);
      continue;
    }
    if (app->dup_group_seen[gid]) {
      continue;
    }

    size_t mc = diskatlas_dup_group_member_count(app->scan, gid);
    if (mc < 2) {
      fill_row_strings(app, nid, col, &pct);
      lp = (gint64)(nid + 1u);
      gtk_tree_store_insert_with_values(
          store, &iter, NULL, -1, 0, col[0], 1, col[1], 2, col[2], 3, col[3], 4, col[4], 5,
          col[5], 6, col[6], 7, col[7], 8, col[8], DA_COL_PCT, pct, DA_COL_LP, lp, -1);
      continue;
    }

    app->dup_group_seen[gid] = 1;
    size_t nmem = 0;
    const size_t *mp = diskatlas_dup_group_members(app->scan, gid, &nmem);
    if (mp == NULL || nmem < 2 || mp[0] >= v.count) {
      fill_row_strings(app, nid, col, &pct);
      lp = (gint64)(nid + 1u);
      gtk_tree_store_insert_with_values(
          store, &iter, NULL, -1, 0, col[0], 1, col[1], 2, col[2], 3, col[3], 4, col[4], 5,
          col[5], 6, col[6], 7, col[7], 8, col[8], DA_COL_PCT, pct, DA_COL_LP, lp, -1);
      continue;
    }
    /* Same as legacy UI: parent row shows the canonical member mp[0] in all columns; expand for others. */
    fill_row_strings(app, mp[0], col, &pct);
    lp = -(gint64)gid;
    gtk_tree_store_insert_with_values(store, &iter, NULL, -1, 0, col[0], 1, col[1], 2, col[2], 3,
                                      col[3], 4, col[4], 5, col[5], 6, col[6], 7, col[7], 8, col[8],
                                      DA_COL_PCT, pct, DA_COL_LP, lp, -1);
    if (nmem > 1) {
      GtkTreeIter ph;
      gtk_tree_store_insert_with_values(
          store, &ph, &iter, -1, 0, " ", 1, "", 2, "", 3, "", 4, "", 5, "", 6, "", 7, "", 8, "",
          DA_COL_PCT, -1, DA_COL_LP, DA_TREE_LP_CHILD_PLACEHOLDER, -1);
    }
  }

  return app->tree_insert_pos < total;
}

void da_tree_on_row_expanded(GtkTreeView *tv, GtkTreeIter *iter, GtkTreePath *path,
                             gpointer user_data) {
  (void)path;
  AppState *app = (AppState *)user_data;
  if (app == NULL || app->store == NULL || app->scan == NULL || tv == NULL || iter == NULL) {
    return;
  }

  GtkTreeModel *model = GTK_TREE_MODEL(app->store);
  gint64 lp = 0;
  gtk_tree_model_get(model, iter, DA_COL_LP, &lp, -1);
  if (lp >= 0) {
    return;
  }

  uint32_t gid = (uint32_t)(-lp);

  GtkTreeIter child_probe;
  if (gtk_tree_model_iter_children(model, &child_probe, iter)) {
    gint64 c_lp = 0;
    gtk_tree_model_get(model, &child_probe, DA_COL_LP, &c_lp, -1);
    if (c_lp == DA_TREE_LP_CHILD_PLACEHOLDER) {
      gtk_tree_store_remove(GTK_TREE_STORE(model), &child_probe);
    } else {
      return;
    }
  }

  size_t nmem = 0;
  const size_t *mp = diskatlas_dup_group_members(app->scan, gid, &nmem);
  scan_results_view_t v = scan_get_results(app->scan);
  if (mp == NULL || nmem == 0 || v.nodes == NULL) {
    return;
  }

  for (size_t k = 1; k < nmem; k++) {
    size_t ni = mp[k];
    if (ni >= v.count) {
      continue;
    }
    GtkTreeIter ch;
    char col[DA_COL_COUNT][512];
    gint pct = -1;
    fill_row_strings(app, ni, col, &pct);
    /* Dup member: show full path in name column (legacy behavior). */
    g_strlcpy(col[0], v.nodes[ni].path, sizeof(col[0]));
    gtk_tree_store_insert_with_values(
        app->store, &ch, iter, -1, 0, col[0], 1, col[1], 2, col[2], 3, col[3], 4, col[4], 5,
        col[5], 6, col[6], 7, col[7], 8, col[8], DA_COL_PCT, pct, DA_COL_LP, (gint64)(ni + 1u), -1);
  }
}

gboolean da_tree_lp_to_scan_nid(AppState *app, gint64 lp, size_t *out_nid) {
  if (out_nid == NULL) {
    return FALSE;
  }
  if (lp == DA_TREE_LP_PLACEHOLDER) {
    *out_nid = SIZE_MAX;
    return TRUE;
  }
  if (app == NULL || app->scan == NULL) {
    return FALSE;
  }
  if (lp < 0) {
    uint32_t gid = (uint32_t)(-lp);
    size_t nmem = 0;
    const size_t *mp = diskatlas_dup_group_members(app->scan, gid, &nmem);
    scan_results_view_t v = scan_get_results(app->scan);
    if (mp != NULL && nmem > 0 && mp[0] < v.count) {
      *out_nid = mp[0];
      return TRUE;
    }
    return FALSE;
  }
  if (lp > 0) {
    *out_nid = (size_t)(lp - 1);
    return TRUE;
  }
  return FALSE;
}
