#include <inttypes.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <glib.h>
#include <gtk/gtk.h>

#include "treemap_widget.h"
#include "diskatlas.h"
#include "file_tree_model.h"
#include "flat_list_model.h"
#include "tree_view_model.h"
#include "format_text.h"
#include "scan_controller.h"
#include "scan_source_combo.h"
#include "ui_window.h"
#include "volumes.h"

#define DA_FILE_VIEW_NOTEBOOK_PAGE 0
#define DA_TREE_VIEW_NOTEBOOK_PAGE 1

static gboolean scan_controller_is_file_view_tab(const AppState *app) {
  if (app == NULL || app->main_notebook == NULL) {
    return TRUE;
  }
  return gtk_notebook_get_current_page(GTK_NOTEBOOK(app->main_notebook)) == DA_FILE_VIEW_NOTEBOOK_PAGE;
}

static gboolean scan_controller_is_tree_view_tab(const AppState *app) {
  if (app == NULL || app->main_notebook == NULL) {
    return FALSE;
  }
  return gtk_notebook_get_current_page(GTK_NOTEBOOK(app->main_notebook)) == DA_TREE_VIEW_NOTEBOOK_PAGE;
}

static void scan_controller_clear_file_view_status(AppState *app) {
  if (app == NULL) {
    return;
  }
  if (app->status_label_left != NULL) {
    gtk_label_set_text(GTK_LABEL(app->status_label_left), "");
  }
  if (app->status_label_center != NULL) {
    gtk_label_set_text(GTK_LABEL(app->status_label_center), "");
  }
  if (app->status_label_right != NULL) {
    gtk_label_set_text(GTK_LABEL(app->status_label_right), "");
  }
}

static void scan_controller_update_status_left_from_selection(AppState *app) {
  if (app == NULL || app->status_label_left == NULL || app->tree == NULL) {
    return;
  }
  if (app->scan == NULL) {
    gtk_label_set_text(GTK_LABEL(app->status_label_left), "");
    return;
  }
  scan_results_view_t v = scan_get_results(app->scan);
  if (v.nodes == NULL) {
    gtk_label_set_text(GTK_LABEL(app->status_label_left), "");
    return;
  }
  GtkTreeSelection *sel = gtk_tree_view_get_selection(GTK_TREE_VIEW(app->tree));
  GtkTreeModel *model = gtk_tree_view_get_model(GTK_TREE_VIEW(app->tree));
  GList *rows = gtk_tree_selection_get_selected_rows(sel, &model);
  if (rows == NULL) {
    gtk_label_set_text(GTK_LABEL(app->status_label_left), "");
    return;
  }
  size_t n_files_sel = 0;
  uint64_t sum_sz = 0;
  uint64_t sum_alloc = 0;
  for (GList *l = rows; l != NULL; l = l->next) {
    GtkTreePath *path = (GtkTreePath *)l->data;
    GtkTreeIter iter;
    if (!gtk_tree_model_get_iter(model, &iter, path)) {
      continue;
    }
    gint64 lp = 0;
    gtk_tree_model_get(model, &iter, DA_COL_LP, &lp, -1);
    size_t nid = 0;
    if (!da_tree_lp_to_scan_nid(app, lp, &nid) || nid == SIZE_MAX || nid >= v.count) {
      continue;
    }
    const file_node_t *node = &v.nodes[nid];
    uint32_t kind = node->attributes & DISKATLAS_NODE_KIND_MASK;
    if (kind == DISKATLAS_NODE_KIND_FILE) {
      n_files_sel++;
    }
    sum_sz += node->size_bytes;
    sum_alloc += node->allocated_bytes;
  }
  g_list_free_full(rows, (GDestroyNotify)gtk_tree_path_free);

  char nfs[48];
  char tsz[72];
  char asz[72];
  char buf[512];
  da_format_uint64_locale((uint64_t)n_files_sel, nfs, sizeof(nfs));
  da_format_bytes(sum_sz, tsz, sizeof(tsz));
  da_format_bytes(sum_alloc, asz, sizeof(asz));
  snprintf(buf, sizeof(buf), "Selected Files: %s, Total Size: %s, Allocated: %s", nfs, tsz, asz);
  gtk_label_set_text(GTK_LABEL(app->status_label_left), buf);
}

static void scan_controller_update_status_right_totals(AppState *app) {
  if (app == NULL || app->status_label_right == NULL) {
    return;
  }
  if (app->scan == NULL) {
    gtk_label_set_text(GTK_LABEL(app->status_label_right), "");
    return;
  }
  scan_progress_t pr = scan_get_progress(app->scan);
  if (!pr.is_complete) {
    gtk_label_set_text(GTK_LABEL(app->status_label_right), "—");
    return;
  }
  if (!app->list_populated) {
    gtk_label_set_text(GTK_LABEL(app->status_label_right), "Loading file list...");
    return;
  }
  scan_results_view_t v = scan_get_results(app->scan);
  if (v.nodes == NULL) {
    gtk_label_set_text(GTK_LABEL(app->status_label_right), "");
    return;
  }
  size_t n_list = da_source_count(app);
  uint64_t sum_sz = 0;
  uint64_t sum_alloc = 0;
  size_t n_files = 0;
  for (size_t i = 0; i < n_list; i++) {
    size_t nid = da_source_at(app, i);
    if (nid >= v.count) {
      continue;
    }
    const file_node_t *node = &v.nodes[nid];
    uint32_t kind = node->attributes & DISKATLAS_NODE_KIND_MASK;
    if (kind == DISKATLAS_NODE_KIND_FILE) {
      n_files++;
    }
    sum_sz += node->size_bytes;
    sum_alloc += node->allocated_bytes;
  }
  char nf[48];
  char tsz[72];
  char asz[72];
  char buf[384];
  da_format_uint64_locale((uint64_t)n_files, nf, sizeof(nf));
  da_format_bytes(sum_sz, tsz, sizeof(tsz));
  da_format_bytes(sum_alloc, asz, sizeof(asz));
  snprintf(buf, sizeof(buf), "(%s files, Total Size: %s, Allocated: %s)", nf, tsz, asz);
  gtk_label_set_text(GTK_LABEL(app->status_label_right), buf);
}

void scan_controller_sync_file_view_status(AppState *app) {
  if (app == NULL || app->status_label_left == NULL) {
    return;
  }
  if (!scan_controller_is_file_view_tab(app)) {
    scan_controller_clear_file_view_status(app);
    return;
  }
  scan_controller_update_status_left_from_selection(app);
  scan_controller_update_status_right_totals(app);
}

static void on_tree_selection_changed(GtkTreeSelection *sel, gpointer user_data) {
  (void)sel;
  AppState *app = (AppState *)user_data;
  if (app != NULL && scan_controller_is_file_view_tab(app)) {
    scan_controller_update_status_left_from_selection(app);
  }
}

static gboolean on_tree_motion(GtkWidget *tv, GdkEventMotion *ev, gpointer user_data) {
  AppState *app = (AppState *)user_data;
  if (app == NULL || app->status_label_center == NULL || !scan_controller_is_file_view_tab(app)) {
    return FALSE;
  }
  GtkTreePath *path = NULL;
  if (!gtk_tree_view_get_path_at_pos(GTK_TREE_VIEW(tv), (gint)ev->x, (gint)ev->y, &path, NULL, NULL,
                                     NULL)) {
    gtk_label_set_text(GTK_LABEL(app->status_label_center), "");
    return FALSE;
  }
  GtkTreeIter iter;
  GtkTreeModel *model = gtk_tree_view_get_model(GTK_TREE_VIEW(tv));
  gchar *pstr = NULL;
  if (gtk_tree_model_get_iter(model, &iter, path)) {
    gtk_tree_model_get(model, &iter, 1, &pstr, -1);
  }
  gtk_tree_path_free(path);
  gtk_label_set_text(GTK_LABEL(app->status_label_center), pstr != NULL ? pstr : "");
  g_free(pstr);
  return FALSE;
}

static gboolean on_tree_leave(GtkWidget *tv, GdkEventCrossing *ev, gpointer user_data) {
  (void)tv;
  (void)ev;
  AppState *app = (AppState *)user_data;
  if (app != NULL && app->status_label_center != NULL && scan_controller_is_file_view_tab(app)) {
    gtk_label_set_text(GTK_LABEL(app->status_label_center), "");
  }
  return FALSE;
}

/* ---- Tree View tab hover handlers ---------------------------------------- */

static gboolean on_tree_view_motion(GtkWidget *tv, GdkEventMotion *ev, gpointer user_data) {
  AppState *app = (AppState *)user_data;
  if (app == NULL || app->status_label_center == NULL || !scan_controller_is_tree_view_tab(app)) {
    return FALSE;
  }
  GtkTreePath *tp = NULL;
  if (!gtk_tree_view_get_path_at_pos(GTK_TREE_VIEW(tv), (gint)ev->x, (gint)ev->y, &tp, NULL,
                                     NULL, NULL)) {
    gtk_label_set_text(GTK_LABEL(app->status_label_center), "");
    return FALSE;
  }
  GtkTreeIter iter;
  GtkTreeModel *model = gtk_tree_view_get_model(GTK_TREE_VIEW(tv));
  gchar *pstr = NULL;
  if (gtk_tree_model_get_iter(model, &iter, tp)) {
    gtk_tree_model_get(model, &iter, DA_TV_COL_PATH, &pstr, -1);
  }
  gtk_tree_path_free(tp);
  gtk_label_set_text(GTK_LABEL(app->status_label_center), pstr != NULL ? pstr : "");
  g_free(pstr);
  return FALSE;
}

static gboolean on_tree_view_leave(GtkWidget *tv, GdkEventCrossing *ev, gpointer user_data) {
  (void)tv;
  (void)ev;
  AppState *app = (AppState *)user_data;
  if (app != NULL && app->status_label_center != NULL && scan_controller_is_tree_view_tab(app)) {
    gtk_label_set_text(GTK_LABEL(app->status_label_center), "");
  }
  return FALSE;
}

/* ---- Tree View selection → status_label_left ------------------------------ */

static void scan_controller_update_status_left_from_tree_view_selection(AppState *app) {
  if (app == NULL || app->status_label_left == NULL || app->tree_view == NULL) {
    return;
  }
  if (app->tree_view_model == NULL) {
    gtk_label_set_text(GTK_LABEL(app->status_label_left), "");
    return;
  }
  GtkTreeSelection *sel = gtk_tree_view_get_selection(GTK_TREE_VIEW(app->tree_view));
  GtkTreeModel *model = gtk_tree_view_get_model(GTK_TREE_VIEW(app->tree_view));
  GList *rows = gtk_tree_selection_get_selected_rows(sel, &model);
  if (rows == NULL) {
    gtk_label_set_text(GTK_LABEL(app->status_label_left), "");
    return;
  }
  uint64_t n_files_sel = 0;
  uint64_t sum_sz = 0;
  uint64_t sum_alloc = 0;
  for (GList *l = rows; l != NULL; l = l->next) {
    GtkTreePath *path = (GtkTreePath *)l->data;
    GtkTreeIter iter;
    if (!gtk_tree_model_get_iter(model, &iter, path)) {
      continue;
    }
    gint64 idx_id = DA_TV_LP_PLACEHOLDER;
    gtk_tree_model_get(model, &iter, DA_TV_COL_IDX_ID, &idx_id, -1);
    if (idx_id == DA_TV_LP_PLACEHOLDER) {
      continue;
    }
    /* Use pre-aggregated entry stats: for dirs these are subtree totals. */
    uint64_t esz = 0, ealloc = 0, efiles = 0;
    if (da_tv_entry_get_stats(app->tree_view_model, idx_id, &esz, &ealloc, &efiles)) {
      n_files_sel += efiles;
      sum_sz      += esz;
      sum_alloc   += ealloc;
    }
  }
  g_list_free_full(rows, (GDestroyNotify)gtk_tree_path_free);

  char nfs[48];
  char tsz[72];
  char asz[72];
  char buf[512];
  da_format_uint64_locale(n_files_sel, nfs, sizeof(nfs));
  da_format_bytes(sum_sz, tsz, sizeof(tsz));
  da_format_bytes(sum_alloc, asz, sizeof(asz));
  snprintf(buf, sizeof(buf), "Selected Files: %s, Total Size: %s, Allocated: %s", nfs, tsz, asz);
  gtk_label_set_text(GTK_LABEL(app->status_label_left), buf);
}

/* ---- Tree View selection → treemap sync ----------------------------------- */

static void on_tree_view_selection_changed(GtkTreeSelection *sel, gpointer user_data) {
  AppState *app = (AppState *)user_data;
  if (app == NULL || !scan_controller_is_tree_view_tab(app)) {
    return;
  }
  scan_controller_update_status_left_from_tree_view_selection(app);

  if (app->treemap_tree_sync_in_progress) {
    return;
  }
  /* Sync last-selected row to treemap. */
  if (app->treemap == NULL || !TREEMAP_IS_WIDGET(app->treemap) || app->scan == NULL) {
    return;
  }
  GtkTreeModel *model = gtk_tree_view_get_model(GTK_TREE_VIEW(app->tree_view));
  GList *rows = gtk_tree_selection_get_selected_rows(sel, &model);

  app->treemap_tree_sync_in_progress = TRUE;
  /* Clear treemap selection first, then add one entry per selected row. */
  treemap_widget_set_selection_by_scan_index(TREEMAP_WIDGET(app->treemap), -1);

  if (rows != NULL && app->scan != NULL) {
    scan_results_view_t v = scan_get_results(app->scan);
    for (GList *l = rows; l != NULL; l = l->next) {
      GtkTreePath *tp = (GtkTreePath *)l->data;
      GtkTreeIter iter;
      if (!gtk_tree_model_get_iter(model, &iter, tp)) {
        continue;
      }
      gchar *pstr = NULL;
      gtk_tree_model_get(model, &iter, DA_TV_COL_PATH, &pstr, -1);
      if (pstr != NULL && v.nodes != NULL) {
        for (size_t ni = 0; ni < v.count; ni++) {
          if (v.nodes[ni].path != NULL &&
#ifdef G_OS_WIN32
              g_ascii_strcasecmp(v.nodes[ni].path, pstr) == 0
#else
              strcmp(v.nodes[ni].path, pstr) == 0
#endif
          ) {
            treemap_widget_add_to_selection_by_scan_index(TREEMAP_WIDGET(app->treemap),
                                                         (gint64)ni);
            break;
          }
        }
      }
      g_free(pstr);
    }
    g_list_free_full(rows, (GDestroyNotify)gtk_tree_path_free);
  }
  app->treemap_tree_sync_in_progress = FALSE;
}

/* ---- treemap → tree_view sync helper ------------------------------------- */

/**
 * Find and expand ancestor rows in tree_view_store for the given path string,
 * then select the row matching target_path.
 */
static void da_tv_select_path(AppState *app, const char *target_path) {
  GtkTreeView *tv;
  GtkTreeModel *model;
  GtkTreeSelection *sel;
  GtkTreeIter root_iter;
  gboolean found = FALSE;

  if (app == NULL || target_path == NULL || app->tree_view == NULL ||
      app->tree_view_store == NULL) {
    return;
  }
  tv    = GTK_TREE_VIEW(app->tree_view);
  model = GTK_TREE_MODEL(app->tree_view_store);
  sel   = gtk_tree_view_get_selection(tv);

  /* Iterate top-level rows; expand as needed; BFS for the target. */
  if (!gtk_tree_model_get_iter_first(model, &root_iter)) {
    return;
  }

  /* Use a queue-based BFS to locate target_path without recursion. */
  GQueue *queue = g_queue_new();
  {
    GtkTreeIter *copy = g_new(GtkTreeIter, 1);
    *copy = root_iter;
    g_queue_push_tail(queue, copy);
    /* Also enqueue siblings of root. */
    GtkTreeIter sib = root_iter;
    while (gtk_tree_model_iter_next(model, &sib)) {
      GtkTreeIter *sc = g_new(GtkTreeIter, 1);
      *sc = sib;
      g_queue_push_tail(queue, sc);
    }
  }

  /* Clear existing selection first so the sync replaces rather than appends. */
  gtk_tree_selection_unselect_all(sel);

  while (!g_queue_is_empty(queue) && !found) {
    GtkTreeIter *cur = (GtkTreeIter *)g_queue_pop_head(queue);
    gchar *pstr = NULL;
    gtk_tree_model_get(model, cur, DA_TV_COL_PATH, &pstr, -1);

    /* Normalise pstr: trim trailing separators for comparison only. */
    gchar *pstr_norm = NULL;
    if (pstr != NULL) {
      pstr_norm = g_strdup(pstr);
      gsize pn = strlen(pstr_norm);
      while (pn > 0 && (pstr_norm[pn - 1] == '/' || pstr_norm[pn - 1] == '\\')) {
        pstr_norm[--pn] = '\0';
      }
    }

    gboolean match = FALSE;
    if (pstr_norm != NULL) {
#ifdef G_OS_WIN32
      match = (g_ascii_strcasecmp(pstr_norm, target_path) == 0);
#else
      match = (strcmp(pstr_norm, target_path) == 0);
#endif
    }
    if (match) {
      /* Found: select and scroll. */
      gtk_tree_selection_select_iter(sel, cur);
      GtkTreePath *tp = gtk_tree_model_get_path(model, cur);
      if (tp != NULL) {
        gtk_tree_view_scroll_to_cell(tv, tp, NULL, FALSE, 0.0f, 0.0f);
        gtk_tree_path_free(tp);
      }
      found = TRUE;
    } else {
      /* Check if target_path could be under this node (node is an ancestor).
       * Use the trimmed length so "D:\" with plen=3→2 correctly matches "D:\foo". */
      gboolean is_ancestor = FALSE;
      if (pstr_norm != NULL) {
        size_t clen = strlen(pstr_norm);
        if (clen > 0) {
#ifdef G_OS_WIN32
          is_ancestor = (g_ascii_strncasecmp(pstr_norm, target_path, (gint)clen) == 0) &&
                        (target_path[clen] == '/' || target_path[clen] == '\\' ||
                         target_path[clen] == '\0');
#else
          is_ancestor = (strncmp(pstr_norm, target_path, clen) == 0) &&
                        (target_path[clen] == '/' || target_path[clen] == '\\' ||
                         target_path[clen] == '\0');
#endif
        }
      }
      if (is_ancestor && gtk_tree_model_iter_has_child(model, cur)) {
        /* Expand row to trigger lazy loading. */
        GtkTreePath *tp = gtk_tree_model_get_path(model, cur);
        if (tp != NULL) {
          gtk_tree_view_expand_row(tv, tp, FALSE);
          gtk_tree_path_free(tp);
        }
        /* Enqueue children. */
        GtkTreeIter child;
        if (gtk_tree_model_iter_children(model, &child, cur)) {
          do {
            GtkTreeIter *cc = g_new(GtkTreeIter, 1);
            *cc = child;
            g_queue_push_tail(queue, cc);
          } while (gtk_tree_model_iter_next(model, &child));
        }
      }
    }
    g_free(pstr_norm);
    g_free(pstr);
    g_free(cur);
  }

  /* Drain remaining queue entries. */
  while (!g_queue_is_empty(queue)) {
    g_free(g_queue_pop_head(queue));
  }
  g_queue_free(queue);
}

static void da_tv_select_path_by_scan_index(AppState *app, gint64 scan_index) {
  if (app == NULL || app->scan == NULL || scan_index < 0) {
    if (app != NULL && app->tree_view != NULL) {
      GtkTreeSelection *sel = gtk_tree_view_get_selection(GTK_TREE_VIEW(app->tree_view));
      gtk_tree_selection_unselect_all(sel);
    }
    return;
  }
  scan_results_view_t v = scan_get_results(app->scan);
  if (v.nodes == NULL || (size_t)scan_index >= v.count) {
    return;
  }
  const char *target = v.nodes[(size_t)scan_index].path;
  if (target != NULL) {
    da_tv_select_path(app, target);
  }
}

static void on_notebook_switch_page(GtkNotebook *nb, GtkWidget *page, guint page_num,
                                    gpointer user_data) {
  (void)nb;
  (void)page;
  (void)page_num;
  AppState *app = (AppState *)user_data;
  if (scan_controller_is_tree_view_tab(app) && app != NULL && app->status_label_right != NULL) {
    gtk_label_set_text(GTK_LABEL(app->status_label_right), "");
  }
  scan_controller_sync_file_view_status(app);
}

static const file_node_t *da_qsort_nodes;

static int cmp_index_by_size_desc(const void *pa, const void *pb);

static int cmp_index_by_size_desc(const void *pa, const void *pb) {
  size_t ia = *(const size_t *)pa;
  size_t ib = *(const size_t *)pb;
  uint64_t sa = da_qsort_nodes[ia].size_bytes;
  uint64_t sb = da_qsort_nodes[ib].size_bytes;
  if (sa > sb) {
    return -1;
  }
  if (sa < sb) {
    return 1;
  }
  return (ia > ib) - (ia < ib);
}

static int rebuild_master_index_list(AppState *app) {
  scan_results_view_t v = scan_get_results(app->scan);
  if (app == NULL || app->scan == NULL || v.nodes == NULL || v.count == 0) {
    free(app->master_indices);
    app->master_indices = NULL;
    app->master_count = 0;
    return 0;
  }
  size_t n_vis = 0;
  for (size_t i = 0; i < v.count; ++i) {
    if (da_node_shown_in_file_view(app, &v, i)) {
      n_vis++;
    }
  }
  if (n_vis == 0) {
    free(app->master_indices);
    app->master_indices = NULL;
    app->master_count = 0;
    return 0;
  }
  size_t *indices = (size_t *)calloc(n_vis, sizeof(size_t));
  if (!indices) {
    return -1;
  }
  size_t j = 0;
  for (size_t i = 0; i < v.count; ++i) {
    if (da_node_shown_in_file_view(app, &v, i)) {
      indices[j++] = i;
    }
  }
  da_qsort_nodes = v.nodes;
  qsort(indices, n_vis, sizeof(size_t), cmp_index_by_size_desc);
  da_qsort_nodes = NULL;
  free(app->master_indices);
  app->master_indices = indices;
  app->master_count = n_vis;
  return 0;
}

static gboolean on_timer_fill_chunk(gpointer data);
static gboolean on_timer_filter_chunk(gpointer data);
static void flat_model_commit_indices(AppState *app);

static void da_treemap_panel_sync_title(AppState *app) {
  if (app == NULL || app->treemap_panel_title == NULL) {
    return;
  }
  if (app->csv_import_active && app->csv_import_path != NULL && app->csv_import_path[0] != '\0') {
    gtk_label_set_text(GTK_LABEL(app->treemap_panel_title),
                       app->import_snapshot_is_raw_mft ? "Top level: <MFT dump>" : "Top level: <CSV File>");
    gtk_widget_set_tooltip_text(app->treemap_panel_title, app->csv_import_path);
    return;
  }
  if (app->scan_root_utf8 != NULL && app->scan_root_utf8[0] != '\0') {
    char buf[768];
    snprintf(buf, sizeof buf, "Top level: %s", app->scan_root_utf8);
    gtk_label_set_text(GTK_LABEL(app->treemap_panel_title), buf);
    gtk_widget_set_tooltip_text(app->treemap_panel_title, app->scan_root_utf8);
  } else {
    gtk_label_set_text(GTK_LABEL(app->treemap_panel_title), "Top level: —");
    gtk_widget_set_tooltip_text(app->treemap_panel_title, NULL);
  }
}

/** Main window title: "{path} - DiskAtlas" when a folder/volume is selected (matches UI default title). */
static void da_main_window_sync_title(AppState *app) {
  static const char k_app_name[] = "DiskAtlas";
  if (app == NULL || app->window == NULL || !GTK_IS_WINDOW(app->window)) {
    return;
  }
  if (app->csv_import_active && app->csv_import_path != NULL && app->csv_import_path[0] != '\0') {
    gchar *bn = g_path_get_basename(app->csv_import_path);
    char title[4096];
    snprintf(title, sizeof(title), "%s - %s", bn != NULL ? bn : app->csv_import_path, k_app_name);
    gtk_window_set_title(GTK_WINDOW(app->window), title);
    g_free(bn);
    return;
  }
  if (app->scan_root_utf8 == NULL || app->scan_root_utf8[0] == '\0') {
    gtk_window_set_title(GTK_WINDOW(app->window), k_app_name);
    return;
  }
  char title[4096];
  snprintf(title, sizeof(title), "%s - %s", app->scan_root_utf8, k_app_name);
  gtk_window_set_title(GTK_WINDOW(app->window), title);
}

static void da_path_to_forward_slashes(char *p) {
  char *q;
  if (p == NULL) {
    return;
  }
  for (q = p; *q != '\0'; q++) {
    if (*q == '\\') {
      *q = '/';
    }
  }
}

static size_t da_common_path_prefix_len(const char *a, const char *b) {
  size_t i;
  for (i = 0; a[i] != '\0' && b[i] != '\0'; i++) {
    char ca = a[i];
    char cb = b[i];
    if (ca == '\\') {
      ca = '/';
    }
    if (cb == '\\') {
      cb = '/';
    }
#if defined(G_OS_WIN32)
    if (g_ascii_tolower((guchar)ca) != g_ascii_tolower((guchar)cb)) {
      break;
    }
#else
    if (ca != cb) {
      break;
    }
#endif
  }
  return i;
}

/** Strip trailing slashes but keep "/" (filesystem root). */
static void da_strip_trailing_slashes_keep_root(char *p) {
  size_t n;
  if (p == NULL) {
    return;
  }
  n = strlen(p);
  while (n > 1u && (p[n - 1u] == '/' || p[n - 1u] == '\\')) {
    p[--n] = '\0';
  }
}

#if defined(G_OS_WIN32)
/** If @a s starts with a drive letter (e.g. "d:/foo"), uppercase that letter for display. */
static void da_win32_uppercase_abs_drive_letter(gchar *s) {
  if (s != NULL && s[0] != '\0' && g_ascii_isalpha((guchar)s[0]) && s[1] == ':') {
    s[0] = (char)g_ascii_toupper((guchar)s[0]);
  }
}
#endif

/**
 * CSV import leaves scan_root empty; treemap_build_tree needs a non-empty root.
 * Uses longest shared directory prefix of all node paths (then dirname fallback).
 */
static gchar *da_derive_treemap_root_from_nodes(const file_node_t *nodes, size_t count) {
  const char *p0;
  gchar *prefix;
  size_t n;
  size_t i;

  if (nodes == NULL || count == 0u) {
    return NULL;
  }
  p0 = NULL;
  for (i = 0; i < count; i++) {
    if (nodes[i].path != NULL && nodes[i].path[0] != '\0') {
      p0 = nodes[i].path;
      break;
    }
  }
  if (p0 == NULL) {
    return NULL;
  }
  prefix = g_strdup(p0);
  da_path_to_forward_slashes(prefix);
  da_strip_trailing_slashes_keep_root(prefix);
  for (i = 0; i < count; i++) {
    const char *p = nodes[i].path;
    gchar *q;
    size_t m;

    if (p == NULL || p[0] == '\0') {
      continue;
    }
    q = g_strdup(p);
    da_path_to_forward_slashes(q);
    da_strip_trailing_slashes_keep_root(q);
    m = da_common_path_prefix_len(prefix, q);
    prefix[m] = '\0';
    g_free(q);
  }
  n = strlen(prefix);
  while (n > 0u && prefix[n - 1u] != '/') {
    n--;
  }
  if (n == 0u) {
    gchar *d = g_path_get_dirname(p0);
    g_free(prefix);
#if defined(G_OS_WIN32)
    da_win32_uppercase_abs_drive_letter(d);
#endif
    return d;
  }
  prefix[n] = '\0';
  da_strip_trailing_slashes_keep_root(prefix);
  if (prefix[0] == '\0') {
    gchar *d;

    g_free(prefix);
    d = g_path_get_dirname(p0);
#if defined(G_OS_WIN32)
    da_win32_uppercase_abs_drive_letter(d);
#endif
    return d;
  }
#if defined(G_OS_WIN32)
  da_win32_uppercase_abs_drive_letter(prefix);
#endif
  return prefix;
}

/**
 * Root path for tree/treemap after CSV import: same Windows volume when all paths share a drive
 * (e.g. "D:\\"); UNC or mixed drives fall back to deepest shared directory from paths.
 */
static gchar *da_derive_csv_import_scan_root(const file_node_t *nodes, size_t count) {
#if defined(G_OS_WIN32)
  int drive = -2; /* -2 unset */
  gboolean any = FALSE;

  if (nodes == NULL || count == 0u) {
    return NULL;
  }
  for (size_t i = 0; i < count; i++) {
    const char *p = nodes[i].path;
    if (p == NULL || p[0] == '\0') {
      continue;
    }
    any = TRUE;
    if (p[0] == '\\' && p[1] == '\\') {
      return da_derive_treemap_root_from_nodes(nodes, count);
    }
    if (!g_ascii_isalpha((guchar)p[0]) || p[1] != ':') {
      return da_derive_treemap_root_from_nodes(nodes, count);
    }
    {
      int d = g_ascii_tolower((guchar)p[0]);
      if (drive == -2) {
        drive = d;
      } else if (drive != d) {
        return da_derive_treemap_root_from_nodes(nodes, count);
      }
    }
  }
  if (any && drive >= 0) {
    return g_strdup_printf("%c:\\", g_ascii_toupper((guchar)drive));
  }
#endif
  return da_derive_treemap_root_from_nodes(nodes, count);
}

static void da_refresh_treemap(AppState *app) {
  da_treemap_panel_sync_title(app);
  if (app != NULL && app->treemap != NULL && TREEMAP_IS_WIDGET(app->treemap)) {
    scan_results_view_t v = {0};
    const char *root_for_treemap = "";
    gchar *derived_root = NULL;

    if (app->scan != NULL) {
      v = scan_get_results(app->scan);
    }
    root_for_treemap = app->scan_root_utf8 != NULL ? app->scan_root_utf8 : "";
    if (root_for_treemap[0] == '\0' && app->csv_import_active && v.nodes != NULL && v.count > 0u) {
      if (app->csv_derived_root_utf8 != NULL && app->csv_derived_root_utf8[0] != '\0') {
        root_for_treemap = app->csv_derived_root_utf8;
      } else {
        derived_root = da_derive_treemap_root_from_nodes(v.nodes, v.count);
        if (derived_root != NULL && derived_root[0] != '\0') {
          root_for_treemap = derived_root;
        }
      }
    }
    treemap_widget_set_data(TREEMAP_WIDGET(app->treemap), root_for_treemap, v.nodes, v.count);
    g_free(derived_root);
  }
}

void scan_controller_notify_scan_root_changed(AppState *app) {
  if (app != NULL && app->scan_root_utf8 != NULL && app->scan_root_utf8[0] != '\0') {
    app->csv_import_active = FALSE;
    g_free(app->csv_import_path);
    app->csv_import_path = NULL;
    g_free(app->csv_derived_root_utf8);
    app->csv_derived_root_utf8 = NULL;
    app->import_snapshot_is_raw_mft = FALSE;
  }
  scan_controller_refresh_volume_labels(app);
  da_refresh_treemap(app);
}

static void da_tv_select_path_by_scan_index(AppState *app, gint64 scan_index);

static void on_treemap_selected(GtkWidget *treemap, gint64 scan_index, gpointer user_data) {
  AppState *app = (AppState *)user_data;
  (void)treemap;
  if (app == NULL) {
    return;
  }
  /* stat_sel_val is owned exclusively by scan_controller_refresh_volume_labels;
   * do not write to it from treemap selection events. */
  if (app->treemap_tree_sync_in_progress) {
    return;
  }
  app->treemap_tree_sync_in_progress = TRUE;
  da_tv_select_path_by_scan_index(app, scan_index);
  app->treemap_tree_sync_in_progress = FALSE;
}

static void on_treemap_hover(GtkWidget *treemap, gint64 scan_index, gpointer user_data) {
  AppState *app = (AppState *)user_data;
  (void)treemap;
  if (app == NULL || app->status_label_center == NULL) {
    return;
  }
  if (scan_index == -1) {
    gtk_label_set_text(GTK_LABEL(app->status_label_center), "");
    return;
  }
  if (scan_index == -2) {
    gtk_label_set_text(GTK_LABEL(app->status_label_center),
                       "Other (merged entries beyond treemap tile cap)");
    return;
  }
  if (scan_index < 0 || app->scan == NULL) {
    gtk_label_set_text(GTK_LABEL(app->status_label_center), "");
    return;
  }
  scan_results_view_t v = scan_get_results(app->scan);
  size_t ix = (size_t)scan_index;
  if (v.nodes == NULL || ix >= v.count) {
    gtk_label_set_text(GTK_LABEL(app->status_label_center), "");
    return;
  }
  const char *p = v.nodes[ix].path;
  gtk_label_set_text(GTK_LABEL(app->status_label_center), p != NULL ? p : "");
}

static void kill_timer(guint *id) {
  if (*id != 0) {
    g_source_remove(*id);
    *id = 0;
  }
}

static void kill_all_timers(AppState *app) {
  kill_timer(&app->timer_scan);
  kill_timer(&app->timer_fill);
  kill_timer(&app->timer_filter);
  kill_timer(&app->timer_search);
}

static void panel_scan_set_text(AppState *app, const char *text) {
  if (app != NULL && app->panel_scan_label != NULL && text != NULL) {
    gtk_label_set_text(GTK_LABEL(app->panel_scan_label), text);
  }
}

static void mft_dump_flow_clear(AppState *app) {
  if (app == NULL) {
    return;
  }
  g_free(app->mft_dump_save_path);
  app->mft_dump_save_path = NULL;
  g_free(app->mft_dump_volume_root_utf8);
  app->mft_dump_volume_root_utf8 = NULL;
  app->mft_dump_run_stream_after_scan = FALSE;
  app->mft_dump_custom_scan_panel = FALSE;
  app->mft_dump_internal_scan = FALSE;
  app->mft_dump_size_total_hint = 0;
  app->mft_dump_banner_after_populate = FALSE;
}

static void scan_progress_set_indeterminate(AppState *app, gboolean on) {
  if (app == NULL || app->progress == NULL) {
    return;
  }
  if (on) {
    gtk_progress_bar_pulse(GTK_PROGRESS_BAR(app->progress));
  }
}

static void scan_progress_set_full(AppState *app) {
  if (app == NULL || app->progress == NULL) {
    return;
  }
  gtk_progress_bar_set_fraction(GTK_PROGRESS_BAR(app->progress), 1.0);
}

static void scan_progress_reset_idle(AppState *app) {
  if (app == NULL || app->progress == NULL) {
    return;
  }
  gtk_progress_bar_set_fraction(GTK_PROGRESS_BAR(app->progress), 0.0);
}

static void enable_scan_button(AppState *app, gboolean enable) {
  if (app != NULL && app->scan_btn != NULL) {
    gtk_widget_set_sensitive(app->scan_btn, enable);
    if (enable) {
      gtk_button_set_label(GTK_BUTTON(app->scan_btn), "Scan");
    }
  }
}

static void scan_button_set_cancelling_mode(AppState *app) {
  if (app == NULL || app->scan_btn == NULL) {
    return;
  }
  gtk_button_set_label(GTK_BUTTON(app->scan_btn), "Cancel");
  gtk_widget_set_sensitive(app->scan_btn, TRUE);
}

static gboolean ensure_filtered_capacity(AppState *app) {
  if (app->filtered_cap >= app->master_count && app->filtered_indices != NULL) {
    return TRUE;
  }
  size_t nc = app->master_count > 256 ? app->master_count : 256;
  size_t *nb = (size_t *)realloc(app->filtered_indices, nc * sizeof(size_t));
  if (nb == NULL) {
    return FALSE;
  }
  app->filtered_indices = nb;
  app->filtered_cap = nc;
  return TRUE;
}

/* Helper: push the current index set (master or filtered) to the flat model. */
static void flat_model_commit_indices(AppState *app) {
  if (app->flat_list_model == NULL) {
    return;
  }
  if (da_view_uses_filtered_pool(app)) {
    flat_list_model_set_indices(app->flat_list_model,
                                app->filtered_indices, app->filtered_count);
  } else {
    flat_list_model_set_indices(app->flat_list_model,
                                app->master_indices, app->master_count);
  }
}

static void apply_search_filter(AppState *app) {
  kill_timer(&app->timer_search);

  if (app == NULL || app->scan == NULL) {
    return;
  }

  if (app->search != NULL) {
    const gchar *t = gtk_entry_get_text(GTK_ENTRY(app->search));
    g_strlcpy(app->filter_text, t ? t : "", sizeof(app->filter_text));
  } else {
    app->filter_text[0] = '\0';
  }

  gboolean want_search = (app->filter_text[0] != '\0');
  gboolean dup_only = da_duplicates_only(app);
  gboolean need_aux_pool = want_search || dup_only;

  kill_timer(&app->timer_filter);
  app->filter_build_running = FALSE;

  if (!app->list_populated || app->master_count == 0) {
    return;
  }

  app->filter_active = want_search;

  if (!need_aux_pool) {
    app->filtered_count = 0;
    app->filter_scan_pos = 0;
    flat_model_commit_indices(app);
    da_refresh_treemap(app);
    scan_controller_sync_file_view_status(app);
    return;
  }

  if (!ensure_filtered_capacity(app)) {
    GtkWidget *d = gtk_message_dialog_new(GTK_WINDOW(app->window), GTK_DIALOG_MODAL,
                                          GTK_MESSAGE_WARNING, GTK_BUTTONS_OK,
                                          "Could not allocate filter buffer.");
    gtk_dialog_run(GTK_DIALOG(d));
    gtk_widget_destroy(d);
    app->filter_active = FALSE;
    if (app->duplicates_only_check != NULL) {
      gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(app->duplicates_only_check), FALSE);
    }
    flat_model_commit_indices(app);
    da_refresh_treemap(app);
    scan_controller_sync_file_view_status(app);
    return;
  }

  app->filtered_count = 0;
  app->filter_scan_pos = 0;
  app->filter_build_running = TRUE;
  scan_controller_sync_file_view_status(app);
  app->timer_filter = g_timeout_add(12, on_timer_filter_chunk, app);
}

static gboolean on_timer_filter_chunk(gpointer data) {
  AppState *app = (AppState *)data;
  if (!da_view_uses_filtered_pool(app) || !app->filter_build_running || app->scan == NULL ||
      app->master_indices == NULL || app->master_count == 0 || app->filtered_indices == NULL) {
    kill_timer(&app->timer_filter);
    app->filter_build_running = FALSE;
    flat_model_commit_indices(app);
    da_refresh_treemap(app);
    scan_controller_sync_file_view_status(app);
    return G_SOURCE_REMOVE;
  }

  scan_results_view_t v = scan_get_results(app->scan);
  if (v.nodes == NULL) {
    kill_timer(&app->timer_filter);
    app->filter_build_running = FALSE;
    flat_model_commit_indices(app);
    da_refresh_treemap(app);
    scan_controller_sync_file_view_status(app);
    return G_SOURCE_REMOVE;
  }

  size_t scanned = 0;
  for (; app->filter_scan_pos < app->master_count && scanned < (size_t)DA_FILTER_BATCH;
       ++scanned, ++app->filter_scan_pos) {
    size_t nid = app->master_indices[app->filter_scan_pos];
    if (da_duplicates_only(app) &&
        v.nodes[nid].duplicate_group_id == DISKATLAS_DUPLICATE_GROUP_NONE) {
      continue;
    }
    if (app->filter_active && !da_utf8_basename_matches_filter(v.nodes[nid].path, app->filter_text)) {
      continue;
    }

    size_t nf = app->filtered_count;
    if (nf >= app->filtered_cap) {
      size_t grow = app->filtered_cap ? app->filtered_cap * 2u : app->master_count;
      if (grow < nf + 1) {
        grow = nf + 1;
      }
      size_t *nb = (size_t *)realloc(app->filtered_indices, grow * sizeof(size_t));
      if (nb == NULL) {
        kill_timer(&app->timer_filter);
        app->filter_build_running = FALSE;
        app->filter_active = FALSE;
        if (app->duplicates_only_check != NULL) {
          gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(app->duplicates_only_check), FALSE);
        }
        GtkWidget *d = gtk_message_dialog_new(GTK_WINDOW(app->window), GTK_DIALOG_MODAL,
                                              GTK_MESSAGE_WARNING, GTK_BUTTONS_OK,
                                              "Out of memory while filtering.");
        gtk_dialog_run(GTK_DIALOG(d));
        gtk_widget_destroy(d);
        flat_model_commit_indices(app);
        da_refresh_treemap(app);
        scan_controller_sync_file_view_status(app);
        return G_SOURCE_REMOVE;
      }
      app->filtered_indices = nb;
      app->filtered_cap = grow;
    }
    app->filtered_indices[nf] = nid;
    app->filtered_count = nf + 1;
  }

  scan_controller_sync_file_view_status(app);

  if (app->filter_scan_pos >= app->master_count) {
    kill_timer(&app->timer_filter);
    app->filter_build_running = FALSE;
    flat_model_commit_indices(app);
    da_refresh_treemap(app);
    scan_controller_sync_file_view_status(app);
    return G_SOURCE_REMOVE;
  }
  return G_SOURCE_CONTINUE;
}

static void begin_populate_list(AppState *app) {
  scan_results_view_t v = scan_get_results(app->scan);

  kill_timer(&app->timer_fill);
  kill_timer(&app->timer_filter);

  /* Clear the flat model immediately (zero rows while loading). */
  if (app->flat_list_model != NULL) {
    flat_list_model_set_indices(app->flat_list_model, NULL, 0);
  }

  free(app->master_indices);
  app->master_indices = NULL;
  app->master_count = 0;

  free(app->filtered_indices);
  app->filtered_indices = NULL;
  app->filtered_cap = 0;
  app->filtered_count = 0;
  app->filter_scan_pos = 0;
  app->filter_active = FALSE;
  app->filter_build_running = FALSE;

  app->populate_total = 0;

  if (v.nodes == NULL || v.count == 0) {
    if (app->mft_dump_banner_after_populate) {
      panel_scan_set_text(app, "MFT data dump complete.");
      app->mft_dump_banner_after_populate = FALSE;
    } else {
      if (app->csv_import_active) {
        panel_scan_set_text(app, app->import_snapshot_is_raw_mft ? "Imported from MFT dump." : "Imported from CSV.");
      } else {
        char finish_pan[256];
        scan_progress_t pr_done = scan_get_progress(app->scan);
        snprintf(finish_pan, sizeof(finish_pan), "Scan complete in %.2f seconds%s",
                 app->last_scan_elapsed_s,
                 pr_done.is_cancel_observed ? " (cancelled)" : "");
        panel_scan_set_text(app, finish_pan);
      }
    }
    app->list_populated = TRUE;
    enable_scan_button(app, TRUE);
    da_refresh_treemap(app);
    scan_controller_sync_file_view_status(app);
    return;
  }

  app->populate_total = v.count;
  app->timer_fill = g_timeout_add(15, on_timer_fill_chunk, app);
}

static gboolean on_timer_fill_chunk(gpointer data) {
  AppState *app = (AppState *)data;
  if (app->scan == NULL || app->populate_total == 0) {
    kill_timer(&app->timer_fill);
    return G_SOURCE_REMOVE;
  }

  scan_results_view_t v = scan_get_results(app->scan);

  if (app->master_indices == NULL) {
    if (v.nodes == NULL || v.count != app->populate_total) {
      kill_timer(&app->timer_fill);
      app->list_populated = TRUE;
      enable_scan_button(app, TRUE);
      scan_controller_sync_file_view_status(app);
      return G_SOURCE_REMOVE;
    }
    panel_scan_set_text(app, "Sorting entries by size…");
    if (rebuild_master_index_list(app) != 0) {
      kill_timer(&app->timer_fill);
      panel_scan_set_text(app, "Could not allocate sort index.");
      app->list_populated = TRUE;
      enable_scan_button(app, TRUE);
      scan_controller_sync_file_view_status(app);
      GtkWidget *d = gtk_message_dialog_new(GTK_WINDOW(app->window), GTK_DIALOG_MODAL,
                                            GTK_MESSAGE_WARNING, GTK_BUTTONS_OK,
                                            "Out of memory while preparing the sorted file list.");
      gtk_dialog_run(GTK_DIALOG(d));
      gtk_widget_destroy(d);
      return G_SOURCE_REMOVE;
    }
    return G_SOURCE_CONTINUE;
  }

  if (v.nodes == NULL || v.count != app->populate_total) {
    kill_timer(&app->timer_fill);
    free(app->master_indices);
    app->master_indices = NULL;
    app->master_count = 0;
    panel_scan_set_text(app, "Scan results changed during list build; list partial or empty.");
    app->list_populated = TRUE;
    enable_scan_button(app, TRUE);
    scan_controller_sync_file_view_status(app);
    return G_SOURCE_REMOVE;
  }

  kill_timer(&app->timer_fill);

  app->list_populated = TRUE;
  char finish_pan[256];
  scan_progress_t pr_done = scan_get_progress(app->scan);
  if (app->mft_dump_banner_after_populate) {
    panel_scan_set_text(app, "MFT data dump complete.");
    app->mft_dump_banner_after_populate = FALSE;
  } else if (app->csv_import_active) {
    panel_scan_set_text(app, app->import_snapshot_is_raw_mft ? "Imported from MFT dump." : "Imported from CSV.");
  } else {
    snprintf(finish_pan, sizeof(finish_pan), "Scan complete in %.2f seconds%s", app->last_scan_elapsed_s,
             pr_done.is_cancel_observed ? " (cancelled)" : "");
    panel_scan_set_text(app, finish_pan);
  }
  enable_scan_button(app, TRUE);

  /* Populate folder tree view (background thread after tv-background-thread task). */
  da_tree_view_populate(app);

  /* Populate flat file-view list — instantaneous, no timer loop needed. */
  const gchar *peek = gtk_entry_get_text(GTK_ENTRY(app->search));
  gboolean search_nonempty = (peek != NULL && peek[0] != '\0');
  if (search_nonempty || da_duplicates_only(app)) {
    apply_search_filter(app);
  } else {
    flat_model_commit_indices(app);
    da_refresh_treemap(app);
    scan_controller_sync_file_view_status(app);
  }
  return G_SOURCE_REMOVE;
}

#if defined(G_OS_WIN32)
static void mft_dump_on_copy_progress(void *user, int pct, uint64_t done, uint64_t total) {
  AppState *app = (AppState *)user;
  char buf[512];
  char a[64], b[64];
  da_format_bytes(done, a, sizeof a);
  da_format_bytes(total, b, sizeof b);
  snprintf(buf, sizeof buf, "Dumping file %d%% (%s/%s)", pct, a, b);
  panel_scan_set_text(app, buf);
  if (app->progress != NULL && total > 0) {
    gtk_progress_bar_set_fraction(GTK_PROGRESS_BAR(app->progress), (gdouble)pct / 100.0);
  }
  /* Dump runs on the UI thread; process pending redraws so the label/bar update live. */
  if (app->panel_scan_label != NULL) {
    gtk_widget_queue_draw(app->panel_scan_label);
  }
  if (app->progress != NULL) {
    gtk_widget_queue_draw(app->progress);
  }
  while (g_main_context_pending(NULL)) {
    (void)g_main_context_iteration(NULL, FALSE);
  }
}

static void mft_dump_run_stream_copy(AppState *app, gchar *vol_owned, gchar *dest_owned) {
  char err[512];
  int rc =
      diskatlas_win32_dump_mft_file(vol_owned, dest_owned, err, sizeof err, mft_dump_on_copy_progress, app);
  g_free(vol_owned);
  if (rc != 0) {
    scan_progress_reset_idle(app);
    GtkWidget *d = gtk_message_dialog_new(GTK_WINDOW(app->window), GTK_DIALOG_MODAL, GTK_MESSAGE_ERROR,
                                          GTK_BUTTONS_OK, "%s", err[0] != '\0' ? err : "MFT dump failed.");
    gtk_dialog_run(GTK_DIALOG(d));
    gtk_widget_destroy(d);
    g_free(dest_owned);
    scan_controller_refresh_volume_labels(app);
    enable_scan_button(app, TRUE);
    return;
  }
  panel_scan_set_text(app, "MFT data dump complete.");
  scan_progress_set_full(app);
  {
    gchar *msg = g_strdup_printf("MFT data dumped to file %s", dest_owned);
    GtkWidget *d = gtk_message_dialog_new(GTK_WINDOW(app->window), GTK_DIALOG_MODAL, GTK_MESSAGE_INFO,
                                          GTK_BUTTONS_OK, "%s", msg);
    g_free(msg);
    gtk_dialog_run(GTK_DIALOG(d));
    gtk_widget_destroy(d);
  }
  g_free(dest_owned);
  scan_controller_refresh_volume_labels(app);
  enable_scan_button(app, TRUE);
}
#endif

static gboolean on_timer_scan_tick(gpointer data) {
  AppState *app = (AppState *)data;
  if (app->scan == NULL) {
    kill_timer(&app->timer_scan);
    da_ui_sync_file_menu_export_csv(app);
    return G_SOURCE_REMOVE;
  }
  scan_progress_t pr = scan_get_progress(app->scan);

  if (!pr.is_complete) {
    char buf[512];
    if (app->mft_dump_custom_scan_panel) {
      uint64_t done = pr.bytes_accounted;
      uint64_t tot = app->mft_dump_size_total_hint;
      int pct = 0;
      if (tot > 0) {
        pct = (int)((done * 100ull) / tot);
        if (pct > 100) {
          pct = 100;
        }
      }
      char a[64], b[64];
      da_format_bytes(done, a, sizeof a);
      if (tot > 0) {
        da_format_bytes(tot, b, sizeof b);
      } else {
        (void)snprintf(b, sizeof b, "—");
      }
      snprintf(buf, sizeof buf, "Dumping file %d%% (%s/%s)", pct, a, b);
      panel_scan_set_text(app, buf);
      scan_progress_set_indeterminate(app, FALSE);
      if (app->progress != NULL && tot > 0) {
        gdouble fr = (gdouble)((double)done / (double)tot);
        if (fr > 1.0) {
          fr = 1.0;
        }
        gtk_progress_bar_set_fraction(GTK_PROGRESS_BAR(app->progress), fr);
      }
    } else {
      char folders_buf[32];
      char files_buf[32];
      da_format_uint64_locale(pr.folder_count, folders_buf, sizeof(folders_buf));
      da_format_uint64_locale(pr.file_count, files_buf, sizeof(files_buf));
      snprintf(buf, sizeof(buf), "Scanning… (Folders: %s  Files: %s)", folders_buf, files_buf);
      panel_scan_set_text(app, buf);
      scan_progress_set_indeterminate(app, TRUE);
    }
    return G_SOURCE_CONTINUE;
  }

  kill_timer(&app->timer_scan);
  da_ui_sync_file_menu_export_csv(app);
  gint64 now = g_get_monotonic_time();
  app->last_scan_elapsed_s = (double)(now - app->scan_start_us) / 1000000.0;

#if defined(G_OS_WIN32)
  if (app->mft_dump_run_stream_after_scan && app->mft_dump_save_path != NULL &&
      app->mft_dump_volume_root_utf8 != NULL) {
    app->mft_dump_custom_scan_panel = FALSE;
    gchar *dest = app->mft_dump_save_path;
    gchar *vol = app->mft_dump_volume_root_utf8;
    app->mft_dump_save_path = NULL;
    app->mft_dump_volume_root_utf8 = NULL;
    app->mft_dump_run_stream_after_scan = FALSE;
    if (pr.is_cancel_observed) {
      g_free(dest);
      g_free(vol);
      scan_progress_set_full(app);
      gtk_button_set_label(GTK_BUTTON(app->scan_btn), "Scan");
      enable_scan_button(app, FALSE);
      begin_populate_list(app);
      return G_SOURCE_REMOVE;
    }
    scan_progress_reset_idle(app);
    app->mft_dump_banner_after_populate = TRUE;
    mft_dump_run_stream_copy(app, vol, dest);
    gtk_button_set_label(GTK_BUTTON(app->scan_btn), "Scan");
    enable_scan_button(app, FALSE);
    begin_populate_list(app);
    return G_SOURCE_REMOVE;
  }
#endif

  scan_progress_set_full(app);
  gtk_button_set_label(GTK_BUTTON(app->scan_btn), "Scan");
  enable_scan_button(app, FALSE);
  begin_populate_list(app);
  return G_SOURCE_REMOVE;
}


static gboolean on_search_debounce(gpointer data) {
  AppState *app = (AppState *)data;
  kill_timer(&app->timer_search);
  apply_search_filter(app);
  return G_SOURCE_REMOVE;
}

static void on_search_changed(GtkEditable *editable, gpointer user_data) {
  (void)editable;
  AppState *app = (AppState *)user_data;
  kill_timer(&app->timer_search);
  app->timer_search = g_timeout_add(DA_SEARCH_DEBOUNCE_MS, on_search_debounce, app);
}

static void start_scan(AppState *app) {
  if (app == NULL || app->scan_root_utf8 == NULL || app->scan_root_utf8[0] == '\0') {
    return;
  }

  if (!app->mft_dump_internal_scan) {
    mft_dump_flow_clear(app);
  }

  app->csv_import_active = FALSE;
  g_free(app->csv_import_path);
  app->csv_import_path = NULL;
  g_free(app->csv_derived_root_utf8);
  app->csv_derived_root_utf8 = NULL;
  app->import_snapshot_is_raw_mft = FALSE;

  kill_all_timers(app);
  if (app->flat_list_model != NULL) {
    flat_list_model_set_indices(app->flat_list_model, NULL, 0);
  }
  da_tree_view_clear(app);
  da_refresh_treemap(app);

  free(app->master_indices);
  app->master_indices = NULL;
  app->master_count = 0;
  free(app->filtered_indices);
  app->filtered_indices = NULL;
  app->filtered_cap = 0;
  app->filtered_count = 0;
  app->filter_scan_pos = 0;
  app->filter_active = FALSE;
  app->filter_build_running = FALSE;
  app->populate_total = 0;
  app->list_populated = FALSE;

  if (app->scan != NULL) {
    if (app->tv_held_scan == app->scan) {
      /* Background tree-view build is using this scan; the GTask callback will free it. */
      app->scan = NULL;
    } else {
      scan_result_free(app->scan);
      app->scan = NULL;
    }
  }
  da_ui_sync_file_menu_export_csv(app);

  scan_options_t opt;
  memset(&opt, 0, sizeof(opt));
  opt.struct_version = DISKATLAS_SCAN_OPTIONS_STRUCT_VERSION;
  opt.flags = 0;
  opt.max_depth = 0;
  opt.io_threads = 0;
  if (app->duplicates_file_combo != NULL) {
    gint dup_mode = gtk_combo_box_get_active(GTK_COMBO_BOX(app->duplicates_file_combo));
    if (dup_mode < 0) {
      dup_mode = 2;
    }
    switch (dup_mode) {
    case 0:
      opt.flags |= DISKATLAS_SCAN_OPTION_SKIP_DUPLICATE_CLUSTERING;
      break;
    case 1:
      break;
    case 2:
    default:
      opt.flags |= DISKATLAS_SCAN_OPTION_DUPLICATE_USE_MTIME;
      break;
    }
  } else {
    opt.flags |= DISKATLAS_SCAN_OPTION_DUPLICATE_USE_MTIME;
  }
  if ((opt.flags & DISKATLAS_SCAN_OPTION_SKIP_DUPLICATE_CLUSTERING) == 0 && app->match_entire_path_radio != NULL &&
      gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(app->match_entire_path_radio))) {
    opt.flags |= DISKATLAS_SCAN_OPTION_DUPLICATE_MATCH_FULL_PATH;
  }
/* FIXME(ntfs-mft): Set to 1 to use raw $MFT scan when elevated; 0 = FindFirst tree until MFT is fixed. */
#ifndef DISKATLAS_APP_ENABLE_NTFS_MFT
#define DISKATLAS_APP_ENABLE_NTFS_MFT 1
#endif
#if defined(G_OS_WIN32) && DISKATLAS_APP_ENABLE_NTFS_MFT
  if (da_win32_is_process_elevated()) {
    opt.flags |= DISKATLAS_SCAN_OPTION_WIN32_NTFS_MFT;
  }
#endif

  app->scan = scan_start(app->scan_root_utf8, &opt);
  if (app->scan == NULL) {
    GtkWidget *d = gtk_message_dialog_new(GTK_WINDOW(app->window), GTK_DIALOG_MODAL,
                                          GTK_MESSAGE_ERROR, GTK_BUTTONS_OK,
                                          "scan_start failed (path or allocator).");
    gtk_dialog_run(GTK_DIALOG(d));
    gtk_widget_destroy(d);
    enable_scan_button(app, TRUE);
    scan_progress_reset_idle(app);
    panel_scan_set_text(app, "Could not start scan.");
    da_ui_sync_file_menu_export_csv(app);
    return;
  }

  da_ui_sync_file_menu_export_csv(app);
  app->scan_start_us = g_get_monotonic_time();
  scan_button_set_cancelling_mode(app);
  scan_progress_set_indeterminate(app, TRUE);
  app->timer_scan = g_timeout_add(120, on_timer_scan_tick, app);
  panel_scan_set_text(app, "Starting scan…");
}

void scan_controller_request_scan(AppState *app) {
  if (app == NULL) {
    return;
  }
  if (app->scan != NULL) {
    scan_progress_t pr = scan_get_progress(app->scan);
    if (!pr.is_complete) {
      scan_cancel(app->scan);
      panel_scan_set_text(app, "Cancelling…");
      return;
    }
  }
  start_scan(app);
}

static void on_scan_clicked(GtkButton *btn, gpointer user_data) {
  (void)btn;
  scan_controller_request_scan((AppState *)user_data);
}

static void on_show_folders_toggled(GtkToggleButton *btn, gpointer user_data) {
  (void)btn;
  AppState *app = (AppState *)user_data;
  if (app == NULL || !app->list_populated || app->scan == NULL) {
    return;
  }

  kill_timer(&app->timer_fill);
  kill_timer(&app->timer_filter);

  panel_scan_set_text(app, "Sorting entries by size…");
  if (rebuild_master_index_list(app) != 0) {
    GtkWidget *d = gtk_message_dialog_new(GTK_WINDOW(app->window), GTK_DIALOG_MODAL,
                                          GTK_MESSAGE_WARNING, GTK_BUTTONS_OK,
                                          "Out of memory while rebuilding the file list.");
    gtk_dialog_run(GTK_DIALOG(d));
    gtk_widget_destroy(d);
    return;
  }

  apply_search_filter(app);
}

static void on_duplicates_only_toggled(GtkToggleButton *btn, gpointer user_data) {
  (void)btn;
  AppState *app = (AppState *)user_data;
  apply_search_filter(app);
}

static void on_combo_display_changed(GtkComboBox *cb, gpointer user_data) {
  (void)cb;
  AppState *app = (AppState *)user_data;
  scan_controller_sync_display_max_combo(app);
  if (app->list_populated) {
    flat_model_commit_indices(app);
  }
  da_refresh_treemap(app);
  scan_controller_sync_file_view_status(app);
}

void scan_controller_sync_display_max_combo(AppState *app) {
  static const size_t caps[] = {0u, 100u, 1000u, 10000u, 100000u};
  const int ncaps = (int)(sizeof(caps) / sizeof(caps[0]));
  if (app->combo_display_max == NULL) {
    return;
  }
  int ix = gtk_combo_box_get_active(GTK_COMBO_BOX(app->combo_display_max));
  if (ix < 0 || ix >= ncaps) {
    ix = 3;
  }
  app->display_max_entries = caps[ix];
}

static uint64_t da_sum_imported_node_sizes(const AppState *app) {
  if (app == NULL || app->scan == NULL) {
    return 0u;
  }
  scan_results_view_t v = scan_get_results(app->scan);
  uint64_t sum = 0;
  for (size_t i = 0; i < v.count; i++) {
    sum += v.nodes[i].size_bytes;
  }
  return sum;
}

void scan_controller_refresh_volume_labels(AppState *app) {
  da_treemap_panel_sync_title(app);
  da_main_window_sync_title(app);
  if (app->csv_import_active && app->csv_import_path != NULL && app->csv_import_path[0] != '\0') {
    uint64_t csv_sum = da_sum_imported_node_sizes(app);
    app->volume_total_bytes = 0u;
    app->volume_pct_denominator_bytes = csv_sum > 0u ? csv_sum : 0u;
    if (app->flat_list_model != NULL) {
      flat_list_model_invalidate(app->flat_list_model);
    }
    gchar *sel = g_strdup_printf("%s %s",
                                   app->import_snapshot_is_raw_mft ? "<MFT File>" : "<CSV File>",
                                   app->csv_import_path);
    if (app->stat_sel_val != NULL) {
      gtk_label_set_text(GTK_LABEL(app->stat_sel_val), sel != NULL ? sel : (app->import_snapshot_is_raw_mft ? "<MFT File>" : "<CSV File>"));
      gtk_label_set_line_wrap(GTK_LABEL(app->stat_sel_val), TRUE);
    }
    g_free(sel);
    if (app->stat_tot_val != NULL) {
      gtk_label_set_text(GTK_LABEL(app->stat_tot_val), "n/a");
    }
    if (app->stat_free_val != NULL) {
      gtk_label_set_text(GTK_LABEL(app->stat_free_val), "n/a");
    }
    if (app->stat_use_val != NULL) {
      char use_line[96];
      da_format_bytes(csv_sum, use_line, sizeof(use_line));
      gtk_label_set_text(GTK_LABEL(app->stat_use_val), use_line);
    }
    scan_controller_sync_file_view_status(app);
    return;
  }
  if (app->stat_sel_val != NULL) {
    gtk_label_set_line_wrap(GTK_LABEL(app->stat_sel_val), FALSE);
  }
  if (app->scan_root_utf8 == NULL || app->scan_root_utf8[0] == '\0') {
    scan_controller_sync_file_view_status(app);
    return;
  }
  uint64_t tot = 0, free_b = 0, used_b = 0;
  if (da_volume_space_for_path(app->scan_root_utf8, &tot, &free_b, &used_b) != 0) {
    if (app->stat_sel_val) {
      gtk_label_set_text(GTK_LABEL(app->stat_sel_val), "—");
    }
    if (app->stat_tot_val) {
      gtk_label_set_text(GTK_LABEL(app->stat_tot_val), "—");
    }
    if (app->stat_use_val) {
      gtk_label_set_text(GTK_LABEL(app->stat_use_val), "—");
    }
    if (app->stat_free_val) {
      gtk_label_set_text(GTK_LABEL(app->stat_free_val), "—");
    }
    app->volume_total_bytes = 0;
    app->volume_pct_denominator_bytes = 0;
    scan_controller_sync_file_view_status(app);
    return;
  }
  app->volume_total_bytes = tot;
  app->volume_pct_denominator_bytes = (used_b > 0u) ? used_b : tot;
  if (app->flat_list_model != NULL) {
    flat_list_model_invalidate(app->flat_list_model);
  }
  char a[64];
  char use_line[160];
  char free_line[160];
  da_format_bytes(tot, a, sizeof(a));
  da_format_bytes_with_pct(used_b, tot, use_line, sizeof(use_line));
  da_format_bytes_with_pct(free_b, tot, free_line, sizeof(free_line));
  if (app->stat_sel_val) {
    gtk_label_set_text(GTK_LABEL(app->stat_sel_val), app->scan_root_utf8);
  }
  if (app->stat_tot_val) {
    gtk_label_set_text(GTK_LABEL(app->stat_tot_val), a);
  }
  if (app->stat_use_val) {
    gtk_label_set_text(GTK_LABEL(app->stat_use_val), use_line);
  }
  if (app->stat_free_val) {
    gtk_label_set_text(GTK_LABEL(app->stat_free_val), free_line);
  }
  scan_controller_sync_file_view_status(app);
}

static void on_tree_view_row_expanded(GtkTreeView *tv, GtkTreeIter *iter, GtkTreePath *path,
                                      gpointer user_data) {
  da_tree_view_on_row_expanded(tv, iter, path, user_data);
}

void scan_controller_attach(AppState *app) {
  g_signal_connect(app->scan_btn, "clicked", G_CALLBACK(on_scan_clicked), app);
  if (app->scan_source_combo != NULL) {
    g_signal_connect(app->scan_source_combo, "changed", G_CALLBACK(da_scan_source_combo_on_changed), app);
  }
  g_signal_connect(app->combo_display_max, "changed", G_CALLBACK(on_combo_display_changed), app);
  if (app->duplicates_only_check != NULL) {
    g_signal_connect(app->duplicates_only_check, "toggled", G_CALLBACK(on_duplicates_only_toggled), app);
  }
  if (app->show_folders_check != NULL) {
    g_signal_connect(app->show_folders_check, "toggled", G_CALLBACK(on_show_folders_toggled), app);
  }
  g_signal_connect(app->search, "changed", G_CALLBACK(on_search_changed), app);
  if (app->tree_view != NULL) {
    g_signal_connect(app->tree_view, "row-expanded", G_CALLBACK(on_tree_view_row_expanded), app);
    g_signal_connect(app->tree_view, "motion-notify-event", G_CALLBACK(on_tree_view_motion), app);
    g_signal_connect(app->tree_view, "leave-notify-event",  G_CALLBACK(on_tree_view_leave),  app);
    {
      GtkTreeSelection *tv_sel = gtk_tree_view_get_selection(GTK_TREE_VIEW(app->tree_view));
      g_signal_connect(tv_sel, "changed", G_CALLBACK(on_tree_view_selection_changed), app);
    }
  }
  if (app->main_notebook != NULL) {
    g_signal_connect(app->main_notebook, "switch-page", G_CALLBACK(on_notebook_switch_page), app);
  }
  {
    GtkTreeSelection *sel = gtk_tree_view_get_selection(GTK_TREE_VIEW(app->tree));
    g_signal_connect(sel, "changed", G_CALLBACK(on_tree_selection_changed), app);
  }
  g_signal_connect(app->tree, "motion-notify-event", G_CALLBACK(on_tree_motion), app);
  g_signal_connect(app->tree, "leave-notify-event", G_CALLBACK(on_tree_leave), app);
  if (app->treemap != NULL && TREEMAP_IS_WIDGET(app->treemap)) {
    treemap_widget_set_selected_callback(TREEMAP_WIDGET(app->treemap), on_treemap_selected, app);
    treemap_widget_set_hover_callback(TREEMAP_WIDGET(app->treemap), on_treemap_hover, app);
  }
}

void scan_controller_fill_scan_options_for_import(AppState *app, scan_options_t *out) {
  if (app == NULL || out == NULL) {
    return;
  }
  memset(out, 0, sizeof(*out));
  out->struct_version = DISKATLAS_SCAN_OPTIONS_STRUCT_VERSION;
  out->flags = 0;
  out->max_depth = 0;
  out->io_threads = 0;
  if (app->duplicates_file_combo != NULL) {
    gint dup_mode = gtk_combo_box_get_active(GTK_COMBO_BOX(app->duplicates_file_combo));
    if (dup_mode < 0) {
      dup_mode = 2;
    }
    switch (dup_mode) {
    case 0:
      out->flags |= DISKATLAS_SCAN_OPTION_SKIP_DUPLICATE_CLUSTERING;
      break;
    case 1:
      break;
    case 2:
    default:
      out->flags |= DISKATLAS_SCAN_OPTION_DUPLICATE_USE_MTIME;
      break;
    }
  } else {
    out->flags |= DISKATLAS_SCAN_OPTION_DUPLICATE_USE_MTIME;
  }
  if ((out->flags & DISKATLAS_SCAN_OPTION_SKIP_DUPLICATE_CLUSTERING) == 0 && app->match_entire_path_radio != NULL &&
      gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(app->match_entire_path_radio))) {
    out->flags |= DISKATLAS_SCAN_OPTION_DUPLICATE_MATCH_FULL_PATH;
  }
}

void scan_controller_apply_imported_scan(AppState *app, scan_result_t *new_scan, const char *snapshot_path_utf8,
                                         gboolean snapshot_layout, gboolean raw_mft_snapshot) {
  if (app == NULL || new_scan == NULL) {
    return;
  }

  scan_progress_t pr = scan_get_progress(new_scan);
  if (!pr.is_complete) {
    scan_result_free(new_scan);
    return;
  }

  kill_all_timers(app);
  if (app->flat_list_model != NULL) {
    flat_list_model_set_indices(app->flat_list_model, NULL, 0);
  }
  da_tree_view_clear(app);
  da_refresh_treemap(app);

  free(app->master_indices);
  app->master_indices = NULL;
  app->master_count = 0;
  free(app->filtered_indices);
  app->filtered_indices = NULL;
  app->filtered_cap = 0;
  app->filtered_count = 0;
  app->filter_scan_pos = 0;
  app->filter_active = FALSE;
  app->filter_build_running = FALSE;
  app->populate_total = 0;
  app->list_populated = FALSE;

  if (app->scan != NULL) {
    if (app->tv_held_scan == app->scan) {
      app->scan = NULL;
    } else {
      scan_result_free(app->scan);
    }
    app->scan = NULL;
  }

  app->scan = new_scan;

  if (snapshot_layout && snapshot_path_utf8 != NULL && snapshot_path_utf8[0] != '\0') {
    app->csv_import_active = TRUE;
    app->import_snapshot_is_raw_mft = raw_mft_snapshot;
    g_free(app->csv_import_path);
    app->csv_import_path = g_strdup(snapshot_path_utf8);
    g_free(app->scan_root_utf8);
    app->scan_root_utf8 = g_strdup("");
    {
      scan_results_view_t vimp = scan_get_results(app->scan);
      g_free(app->csv_derived_root_utf8);
      app->csv_derived_root_utf8 = da_derive_csv_import_scan_root(vimp.nodes, vimp.count);
    }
  } else {
    app->csv_import_active = FALSE;
    app->import_snapshot_is_raw_mft = FALSE;
    g_free(app->csv_import_path);
    app->csv_import_path = NULL;
    g_free(app->csv_derived_root_utf8);
    app->csv_derived_root_utf8 = NULL;
    if (snapshot_path_utf8 != NULL) {
      g_free(app->scan_root_utf8);
      app->scan_root_utf8 = g_strdup(snapshot_path_utf8);
    }
  }

  scan_progress_set_full(app);
  app->last_scan_elapsed_s = 0.0;
  panel_scan_set_text(app, raw_mft_snapshot ? "Imported from MFT dump." : "Imported from CSV.");
  if (app->scan_btn != NULL) {
    gtk_button_set_label(GTK_BUTTON(app->scan_btn), "Scan");
  }

  scan_controller_refresh_volume_labels(app);
  da_scan_source_combo_rebuild(app);
  /* Refresh treemap/list chrome now that scan_root / csv_derived / import flags are set (timer may run later). */
  da_refresh_treemap(app);
  if (app->treemap != NULL) {
    gtk_widget_queue_draw(app->treemap);
  }
  da_ui_sync_file_menu_export_csv(app);
  begin_populate_list(app);
}

typedef struct {
  size_t nid;
  uint64_t size_disp;
  char *path_copy;
} DaCopyRow;

static void da_copy_row_clear_one(void *elt) {
  DaCopyRow *r = (DaCopyRow *)elt;
  g_free(r->path_copy);
  r->path_copy = NULL;
}

static size_t da_find_scan_nid_for_utf8_path(const scan_results_view_t *v, const gchar *path) {
  if (path == NULL || v == NULL || v->nodes == NULL) {
    return SIZE_MAX;
  }
  for (size_t ni = 0; ni < v->count; ni++) {
    const char *np = v->nodes[ni].path;
    if (np == NULL) {
      continue;
    }
#ifdef G_OS_WIN32
    if (g_ascii_strcasecmp(np, path) == 0) {
      return ni;
    }
#else
    if (strcmp(np, path) == 0) {
      return ni;
    }
#endif
  }
  return SIZE_MAX;
}

static int da_copy_row_cmp(const void *a, const void *b) {
  const DaCopyRow *ra = (const DaCopyRow *)a;
  const DaCopyRow *rb = (const DaCopyRow *)b;
#ifdef G_OS_WIN32
  return g_ascii_strcasecmp(ra->path_copy, rb->path_copy);
#else
  return strcmp(ra->path_copy, rb->path_copy);
#endif
}

static gboolean da_copy_rows_has_nid(const GArray *rows, size_t nid) {
  for (guint i = 0; i < rows->len; i++) {
    if (g_array_index(rows, DaCopyRow, i).nid == nid) {
      return TRUE;
    }
  }
  return FALSE;
}

static void da_copy_rows_push_unique(GArray *rows, size_t nid, uint64_t size_disp, const char *path_utf8) {
  if (nid == SIZE_MAX) {
    return;
  }
  if (da_copy_rows_has_nid(rows, nid)) {
    return;
  }
  DaCopyRow row;
  row.nid = nid;
  row.size_disp = size_disp;
  row.path_copy = g_strdup(path_utf8 != NULL ? path_utf8 : "");
  g_array_append_val(rows, row);
}

void scan_controller_copy_scan_paths_sizes_to_clipboard(AppState *app) {
  if (app == NULL || app->window == NULL || app->scan == NULL) {
    return;
  }
  scan_progress_t pr = scan_get_progress(app->scan);
  if (!pr.is_complete) {
    return;
  }
  scan_results_view_t v = scan_get_results(app->scan);
  if (v.nodes == NULL) {
    return;
  }

  GArray *rows = g_array_new(FALSE, FALSE, sizeof(DaCopyRow));
  g_array_set_clear_func(rows, da_copy_row_clear_one);

  if (scan_controller_is_file_view_tab(app)) {
    GtkTreeSelection *sel = gtk_tree_view_get_selection(GTK_TREE_VIEW(app->tree));
    GtkTreeModel *model = gtk_tree_view_get_model(GTK_TREE_VIEW(app->tree));
    GList *plist = gtk_tree_selection_get_selected_rows(sel, &model);
    GArray *tn = NULL;
    gboolean treemap_used = FALSE;

    if (app->treemap != NULL && TREEMAP_IS_WIDGET(app->treemap)) {
      tn = g_array_new(FALSE, FALSE, sizeof(size_t));
      treemap_widget_append_selected_scan_indices(TREEMAP_WIDGET(app->treemap), tn);
      if (tn->len > 0) {
        treemap_used = TRUE;
        for (guint i = 0; i < tn->len; i++) {
          size_t nid = g_array_index(tn, size_t, i);
          if (nid < v.count) {
            const file_node_t *node = &v.nodes[nid];
            const char *p = node->path != NULL ? node->path : "";
            da_copy_rows_push_unique(rows, nid, node->size_bytes, p);
          }
        }
      }
    }

    if (!treemap_used && plist != NULL) {
      for (GList *l = plist; l != NULL; l = l->next) {
        GtkTreeIter it;
        if (!gtk_tree_model_get_iter(model, &it, (GtkTreePath *)l->data)) {
          continue;
        }
        gint64 lp = 0;
        gtk_tree_model_get(model, &it, DA_COL_LP, &lp, -1);
        size_t nid = SIZE_MAX;
        if (!da_tree_lp_to_scan_nid(app, lp, &nid) || nid == SIZE_MAX || nid >= v.count) {
          continue;
        }
        const file_node_t *node = &v.nodes[nid];
        const char *p = node->path != NULL ? node->path : "";
        da_copy_rows_push_unique(rows, nid, node->size_bytes, p);
      }
    }
    if (plist != NULL) {
      g_list_free_full(plist, (GDestroyNotify)gtk_tree_path_free);
    }
    if (tn != NULL) {
      g_array_free(tn, TRUE);
    }
    if (rows->len == 0) {
      for (size_t i = 0; i < da_source_count(app); i++) {
        size_t nid = da_source_at(app, i);
        if (nid >= v.count) {
          continue;
        }
        const file_node_t *node = &v.nodes[nid];
        const char *p = node->path != NULL ? node->path : "";
        da_copy_rows_push_unique(rows, nid, node->size_bytes, p);
      }
      if (rows->len == 0) {
        for (size_t nid = 0; nid < v.count; nid++) {
          const file_node_t *node = &v.nodes[nid];
          const char *p = node->path != NULL ? node->path : "";
          da_copy_rows_push_unique(rows, nid, node->size_bytes, p);
        }
      }
    }
  } else {
    if (app->tree_view == NULL) {
      g_array_free(rows, TRUE);
      return;
    }
    GtkTreeSelection *sel = gtk_tree_view_get_selection(GTK_TREE_VIEW(app->tree_view));
    GtkTreeModel *model = gtk_tree_view_get_model(GTK_TREE_VIEW(app->tree_view));
    GList *plist = gtk_tree_selection_get_selected_rows(sel, &model);
    gboolean any_tv = FALSE;
    if (plist != NULL) {
      for (GList *l = plist; l != NULL; l = l->next) {
        GtkTreeIter it;
        if (!gtk_tree_model_get_iter(model, &it, (GtkTreePath *)l->data)) {
          continue;
        }
        gint64 idx_id = DA_TV_LP_PLACEHOLDER;
        gtk_tree_model_get(model, &it, DA_TV_COL_IDX_ID, &idx_id, -1);
        if (idx_id == DA_TV_LP_PLACEHOLDER || app->tree_view_model == NULL) {
          continue;
        }
        uint64_t esz = 0, ealloc = 0, efiles = 0;
        if (!da_tv_entry_get_stats(app->tree_view_model, idx_id, &esz, &ealloc, &efiles)) {
          continue;
        }
        gchar *pstr = NULL;
        gtk_tree_model_get(model, &it, DA_TV_COL_PATH, &pstr, -1);
        size_t nid = da_find_scan_nid_for_utf8_path(&v, pstr);
        g_free(pstr);
        if (nid == SIZE_MAX) {
          continue;
        }
        const char *p = v.nodes[nid].path != NULL ? v.nodes[nid].path : "";
        da_copy_rows_push_unique(rows, nid, esz, p);
        any_tv = TRUE;
      }
      g_list_free_full(plist, (GDestroyNotify)gtk_tree_path_free);
    }
    if (!any_tv) {
      for (size_t nid = 0; nid < v.count; nid++) {
        const file_node_t *node = &v.nodes[nid];
        const char *p = node->path != NULL ? node->path : "";
        da_copy_rows_push_unique(rows, nid, node->size_bytes, p);
      }
    }
  }

  if (rows->len == 0) {
    g_array_free(rows, TRUE);
    return;
  }

  qsort(rows->data, rows->len, sizeof(DaCopyRow), da_copy_row_cmp);

  int maxw = 4;
  char szbuf[96];
  for (guint i = 0; i < rows->len; i++) {
    const DaCopyRow *row = &g_array_index(rows, DaCopyRow, i);
    da_format_bytes(row->size_disp, szbuf, sizeof szbuf);
    int w = (int)strlen(szbuf);
    if (w > maxw) {
      maxw = w;
    }
  }

  GString *gs = g_string_new(NULL);
  for (guint i = 0; i < rows->len; i++) {
    const DaCopyRow *row = &g_array_index(rows, DaCopyRow, i);
    da_format_bytes(row->size_disp, szbuf, sizeof szbuf);
    g_string_append_printf(gs, "%*s  %s\n", maxw, szbuf, row->path_copy);
  }

  GtkClipboard *cb = gtk_widget_get_clipboard(app->window, GDK_SELECTION_CLIPBOARD);
  gtk_clipboard_set_text(cb, gs->str, -1);
  g_string_free(gs, TRUE);
  g_array_free(rows, TRUE);
}

#if defined(G_OS_WIN32)
void scan_controller_begin_mft_dump_flow(AppState *app, const gchar *volume_root_utf8,
                                         const gchar *dest_path_utf8, gboolean need_scan) {
  if (app == NULL || volume_root_utf8 == NULL || dest_path_utf8 == NULL) {
    return;
  }
  mft_dump_flow_clear(app);
  uint64_t tot = 0;
  uint64_t free_b = 0;
  uint64_t used_b = 0;
  if (da_volume_space_for_path(volume_root_utf8, &tot, &free_b, &used_b) == 0) {
    app->mft_dump_size_total_hint = tot;
  } else {
    app->mft_dump_size_total_hint = 0;
  }

  if (!need_scan) {
    mft_dump_run_stream_copy(app, g_strdup(volume_root_utf8), g_strdup(dest_path_utf8));
    return;
  }

  app->mft_dump_save_path = g_strdup(dest_path_utf8);
  app->mft_dump_volume_root_utf8 = g_strdup(volume_root_utf8);
  app->mft_dump_run_stream_after_scan = TRUE;
  app->mft_dump_custom_scan_panel = TRUE;
  app->mft_dump_internal_scan = TRUE;
  g_free(app->scan_root_utf8);
  app->scan_root_utf8 = g_strdup(volume_root_utf8);
  scan_controller_notify_scan_root_changed(app);
  start_scan(app);
  app->mft_dump_internal_scan = FALSE;
  if (app->scan == NULL) {
    mft_dump_flow_clear(app);
  }
}
#endif

void scan_controller_detach(AppState *app) {
  kill_all_timers(app);
  mft_dump_flow_clear(app);
}
