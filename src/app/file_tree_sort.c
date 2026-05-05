#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include <glib.h>
#include <gtk/gtk.h>

#include "diskatlas.h"
#include "app_state.h"
#include "file_tree_sort.h"

/** Same as file_tree_model.c placeholder row LP. */
#define DA_TREE_LP_CHILD_PLACEHOLDER ((gint64)INT64_MIN)

static uint64_t dup_group_total_size(scan_result_t *scan, const scan_results_view_t *v, uint32_t gid) {
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

static gboolean lp_to_nid(gint64 lp, AppState *app, size_t *out_nid) {
  if (lp == DA_TREE_LP_CHILD_PLACEHOLDER) {
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

static const file_node_t *node_for_iter(GtkTreeModel *model, GtkTreeIter *it, AppState *app) {
  gint64 lp = 0;
  gtk_tree_model_get(model, it, DA_COL_LP, &lp, -1);
  size_t nid = 0;
  if (!lp_to_nid(lp, app, &nid)) {
    return NULL;
  }
  if (nid == SIZE_MAX) {
    return NULL;
  }
  scan_results_view_t v = scan_get_results(app->scan);
  if (app->scan == NULL || v.nodes == NULL || nid >= v.count) {
    return NULL;
  }
  return &v.nodes[nid];
}

static gint cmp_placeholder(GtkTreeModel *model, gboolean a_ph, gboolean b_ph) {
  if (!a_ph && !b_ph) {
    return 0;
  }
  if (a_ph && b_ph) {
    return 0;
  }
  GtkSortType ord = GTK_SORT_ASCENDING;
  gint sc = 0;
  gtk_tree_sortable_get_sort_column_id(GTK_TREE_SORTABLE(model), &sc, &ord);
  if (a_ph && !b_ph) {
    return (ord == GTK_SORT_ASCENDING) ? 1 : -1;
  }
  return (ord == GTK_SORT_ASCENDING) ? -1 : 1;
}

static gint da_sort_col_name(GtkTreeModel *model, GtkTreeIter *a, GtkTreeIter *b, gpointer data) {
  AppState *app = data;
  (void)app;
  gint64 lp_a = 0, lp_b = 0;
  gtk_tree_model_get(model, a, DA_COL_LP, &lp_a, -1);
  gtk_tree_model_get(model, b, DA_COL_LP, &lp_b, -1);
  gboolean ph_a = (lp_a == DA_TREE_LP_CHILD_PLACEHOLDER);
  gboolean ph_b = (lp_b == DA_TREE_LP_CHILD_PLACEHOLDER);
  gint c = cmp_placeholder(model, ph_a, ph_b);
  if (c != 0) {
    return c;
  }
  gchar *sa = NULL, *sb = NULL;
  gtk_tree_model_get(model, a, 0, &sa, -1);
  gtk_tree_model_get(model, b, 0, &sb, -1);
  gint r = g_utf8_collate(sa != NULL ? sa : "", sb != NULL ? sb : "");
  g_free(sa);
  g_free(sb);
  return r;
}

static gint da_sort_col_path(GtkTreeModel *model, GtkTreeIter *a, GtkTreeIter *b, gpointer data) {
  AppState *app = data;
  (void)app;
  gint64 lp_a = 0, lp_b = 0;
  gtk_tree_model_get(model, a, DA_COL_LP, &lp_a, -1);
  gtk_tree_model_get(model, b, DA_COL_LP, &lp_b, -1);
  gboolean ph_a = (lp_a == DA_TREE_LP_CHILD_PLACEHOLDER);
  gboolean ph_b = (lp_b == DA_TREE_LP_CHILD_PLACEHOLDER);
  gint c = cmp_placeholder(model, ph_a, ph_b);
  if (c != 0) {
    return c;
  }
  gchar *sa = NULL, *sb = NULL;
  gtk_tree_model_get(model, a, 1, &sa, -1);
  gtk_tree_model_get(model, b, 1, &sb, -1);
  gint r = g_utf8_collate(sa != NULL ? sa : "", sb != NULL ? sb : "");
  g_free(sa);
  g_free(sb);
  return r;
}

static gint da_sort_col_pct(GtkTreeModel *model, GtkTreeIter *a, GtkTreeIter *b, gpointer data) {
  (void)data;
  gint pa = 0, pb = 0;
  gtk_tree_model_get(model, a, DA_COL_PCT, &pa, -1);
  gtk_tree_model_get(model, b, DA_COL_PCT, &pb, -1);
  gboolean na = (pa < 0), nb = (pb < 0);
  if (na && nb) {
    return 0;
  }
  if (na || nb) {
    GtkSortType ord = GTK_SORT_ASCENDING;
    gint sc2 = 0;
    gtk_tree_sortable_get_sort_column_id(GTK_TREE_SORTABLE(model), &sc2, &ord);
    if (na && !nb) {
      return (ord == GTK_SORT_ASCENDING) ? 1 : -1;
    }
    return (ord == GTK_SORT_ASCENDING) ? -1 : 1;
  }
  if (pa < pb) {
    return -1;
  }
  if (pa > pb) {
    return 1;
  }
  return 0;
}

static gint da_sort_col_size(GtkTreeModel *model, GtkTreeIter *a, GtkTreeIter *b, gpointer data) {
  AppState *app = data;
  const file_node_t *na = node_for_iter(model, a, app);
  const file_node_t *nb = node_for_iter(model, b, app);
  gint64 lp_a = 0, lp_b = 0;
  gtk_tree_model_get(model, a, DA_COL_LP, &lp_a, -1);
  gtk_tree_model_get(model, b, DA_COL_LP, &lp_b, -1);
  gboolean ph_a = (lp_a == DA_TREE_LP_CHILD_PLACEHOLDER || na == NULL);
  gboolean ph_b = (lp_b == DA_TREE_LP_CHILD_PLACEHOLDER || nb == NULL);
  gint c = cmp_placeholder(model, ph_a, ph_b);
  if (c != 0) {
    return c;
  }
  if (na == NULL || nb == NULL) {
    return 0;
  }
  if (na->size_bytes < nb->size_bytes) {
    return -1;
  }
  if (na->size_bytes > nb->size_bytes) {
    return 1;
  }
  return 0;
}

static gint da_sort_col_alloc(GtkTreeModel *model, GtkTreeIter *a, GtkTreeIter *b, gpointer data) {
  AppState *app = data;
  const file_node_t *na = node_for_iter(model, a, app);
  const file_node_t *nb = node_for_iter(model, b, app);
  gint64 lp_a = 0, lp_b = 0;
  gtk_tree_model_get(model, a, DA_COL_LP, &lp_a, -1);
  gtk_tree_model_get(model, b, DA_COL_LP, &lp_b, -1);
  gboolean ph_a = (lp_a == DA_TREE_LP_CHILD_PLACEHOLDER || na == NULL);
  gboolean ph_b = (lp_b == DA_TREE_LP_CHILD_PLACEHOLDER || nb == NULL);
  gint c = cmp_placeholder(model, ph_a, ph_b);
  if (c != 0) {
    return c;
  }
  if (na == NULL || nb == NULL) {
    return 0;
  }
  if (na->allocated_bytes < nb->allocated_bytes) {
    return -1;
  }
  if (na->allocated_bytes > nb->allocated_bytes) {
    return 1;
  }
  return 0;
}

static gint da_sort_col_mtime(GtkTreeModel *model, GtkTreeIter *a, GtkTreeIter *b, gpointer data) {
  AppState *app = data;
  const file_node_t *na = node_for_iter(model, a, app);
  const file_node_t *nb = node_for_iter(model, b, app);
  gint64 lp_a = 0, lp_b = 0;
  gtk_tree_model_get(model, a, DA_COL_LP, &lp_a, -1);
  gtk_tree_model_get(model, b, DA_COL_LP, &lp_b, -1);
  gboolean ph_a = (lp_a == DA_TREE_LP_CHILD_PLACEHOLDER || na == NULL);
  gboolean ph_b = (lp_b == DA_TREE_LP_CHILD_PLACEHOLDER || nb == NULL);
  gint c = cmp_placeholder(model, ph_a, ph_b);
  if (c != 0) {
    return c;
  }
  if (na == NULL || nb == NULL) {
    return 0;
  }
  if (na->mtime_unix_ns < nb->mtime_unix_ns) {
    return -1;
  }
  if (na->mtime_unix_ns > nb->mtime_unix_ns) {
    return 1;
  }
  return 0;
}

static gint da_sort_col_dup_count(GtkTreeModel *model, GtkTreeIter *a, GtkTreeIter *b, gpointer data) {
  AppState *app = data;
  const file_node_t *na = node_for_iter(model, a, app);
  const file_node_t *nb = node_for_iter(model, b, app);
  gint64 lp_a = 0, lp_b = 0;
  gtk_tree_model_get(model, a, DA_COL_LP, &lp_a, -1);
  gtk_tree_model_get(model, b, DA_COL_LP, &lp_b, -1);
  gboolean ph_a = (lp_a == DA_TREE_LP_CHILD_PLACEHOLDER || na == NULL);
  gboolean ph_b = (lp_b == DA_TREE_LP_CHILD_PLACEHOLDER || nb == NULL);
  gint c = cmp_placeholder(model, ph_a, ph_b);
  if (c != 0) {
    return c;
  }
  if (na == NULL || nb == NULL || app->scan == NULL) {
    return 0;
  }
  size_t ca = 0, cb = 0;
  if (na->duplicate_group_id != DISKATLAS_DUPLICATE_GROUP_NONE) {
    size_t mc = diskatlas_dup_group_member_count(app->scan, na->duplicate_group_id);
    ca = mc > 0 ? mc - 1u : 0u;
  }
  if (nb->duplicate_group_id != DISKATLAS_DUPLICATE_GROUP_NONE) {
    size_t mc = diskatlas_dup_group_member_count(app->scan, nb->duplicate_group_id);
    cb = mc > 0 ? mc - 1u : 0u;
  }
  if (ca < cb) {
    return -1;
  }
  if (ca > cb) {
    return 1;
  }
  return 0;
}

static gint da_sort_col_dup_size(GtkTreeModel *model, GtkTreeIter *a, GtkTreeIter *b, gpointer data) {
  AppState *app = data;
  const file_node_t *na = node_for_iter(model, a, app);
  const file_node_t *nb = node_for_iter(model, b, app);
  gint64 lp_a = 0, lp_b = 0;
  gtk_tree_model_get(model, a, DA_COL_LP, &lp_a, -1);
  gtk_tree_model_get(model, b, DA_COL_LP, &lp_b, -1);
  gboolean ph_a = (lp_a == DA_TREE_LP_CHILD_PLACEHOLDER || na == NULL);
  gboolean ph_b = (lp_b == DA_TREE_LP_CHILD_PLACEHOLDER || nb == NULL);
  gint c = cmp_placeholder(model, ph_a, ph_b);
  if (c != 0) {
    return c;
  }
  if (na == NULL || nb == NULL || app->scan == NULL) {
    return 0;
  }
  scan_results_view_t v = scan_get_results(app->scan);
  uint64_t sa = 0, sb = 0;
  if (na->duplicate_group_id != DISKATLAS_DUPLICATE_GROUP_NONE) {
    sa = dup_group_total_size(app->scan, &v, na->duplicate_group_id);
  }
  if (nb->duplicate_group_id != DISKATLAS_DUPLICATE_GROUP_NONE) {
    sb = dup_group_total_size(app->scan, &v, nb->duplicate_group_id);
  }
  if (sa < sb) {
    return -1;
  }
  if (sa > sb) {
    return 1;
  }
  return 0;
}

static gint da_sort_col_attr(GtkTreeModel *model, GtkTreeIter *a, GtkTreeIter *b, gpointer data) {
  AppState *app = data;
  const file_node_t *na = node_for_iter(model, a, app);
  const file_node_t *nb = node_for_iter(model, b, app);
  gint64 lp_a = 0, lp_b = 0;
  gtk_tree_model_get(model, a, DA_COL_LP, &lp_a, -1);
  gtk_tree_model_get(model, b, DA_COL_LP, &lp_b, -1);
  gboolean ph_a = (lp_a == DA_TREE_LP_CHILD_PLACEHOLDER || na == NULL);
  gboolean ph_b = (lp_b == DA_TREE_LP_CHILD_PLACEHOLDER || nb == NULL);
  gint c = cmp_placeholder(model, ph_a, ph_b);
  if (c != 0) {
    return c;
  }
  if (na == NULL || nb == NULL) {
    return 0;
  }
  if (na->win32_attributes < nb->win32_attributes) {
    return -1;
  }
  if (na->win32_attributes > nb->win32_attributes) {
    return 1;
  }
  return 0;
}

void da_file_tree_install_sorting(GtkTreeView *tv, GtkTreeStore *store, AppState *app) {
  (void)tv;
  GtkTreeSortable *sortable = GTK_TREE_SORTABLE(store);
  gtk_tree_sortable_set_sort_func(sortable, 0, da_sort_col_name, app, NULL);
  gtk_tree_sortable_set_sort_func(sortable, 1, da_sort_col_path, app, NULL);
  gtk_tree_sortable_set_sort_func(sortable, DA_COL_PCT, da_sort_col_pct, app, NULL);
  gtk_tree_sortable_set_sort_func(sortable, 3, da_sort_col_size, app, NULL);
  gtk_tree_sortable_set_sort_func(sortable, 4, da_sort_col_alloc, app, NULL);
  gtk_tree_sortable_set_sort_func(sortable, 5, da_sort_col_mtime, app, NULL);
  gtk_tree_sortable_set_sort_func(sortable, 6, da_sort_col_dup_count, app, NULL);
  gtk_tree_sortable_set_sort_func(sortable, 7, da_sort_col_dup_size, app, NULL);
  gtk_tree_sortable_set_sort_func(sortable, 8, da_sort_col_attr, app, NULL);
}
