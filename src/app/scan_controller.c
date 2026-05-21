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
#include "dm_mime_db.h"
#include "file_type_view.h"
#include "file_tree_model.h"
#include "flat_list_model.h"
#include "tree_view_model.h"
#include "format_text.h"
#include "scan_controller.h"
#include "scan_source_combo.h"
#include "ui_window.h"
#include "volumes.h"
#include "diskatlas_ini.h"
#include "file_ops.h"
#include "da_fs_monitor.h"
#include "da_drag_drop.h"
#include "da_message_dialog.h"

/* Must match main_notebook child order in diskatlas_window.ui: page 0 = Tree View, page 1 = File View. */
#define DA_TREE_VIEW_NOTEBOOK_PAGE 0
#define DA_FILE_VIEW_NOTEBOOK_PAGE 1

#define DA_SEARCH_HISTORY_MAX_ENTRIES 64

static void kill_timer(guint *id);
static void apply_search_filter(AppState *app);
static void on_search_combo_changed(GtkComboBox *cb, gpointer user_data);
static void on_search_changed(GtkEditable *editable, gpointer user_data);
static void on_search_entry_activate(GtkEntry *entry, gpointer user_data);

static GtkEntry *da_file_view_search_find_entry_widget(GtkWidget *widget) {
  if (widget == NULL) {
    return NULL;
  }
  if (GTK_IS_ENTRY(widget)) {
    return GTK_ENTRY(widget);
  }
  if (GTK_IS_CONTAINER(widget)) {
    for (GList *l = gtk_container_get_children(GTK_CONTAINER(widget)); l != NULL; l = l->next) {
      GtkEntry *e = da_file_view_search_find_entry_widget(GTK_WIDGET(l->data));
      if (e != NULL) {
        return e;
      }
    }
  }
  return NULL;
}

GtkEntry *da_file_view_search_get_entry(const AppState *app) {
  if (app == NULL || app->search == NULL) {
    return NULL;
  }
  if (GTK_IS_COMBO_BOX(app->search)) {
    /* gtk_combo_box_get_entry() is GTK 3.22+; walk the combo's child tree instead. */
    return da_file_view_search_find_entry_widget(app->search);
  }
  if (GTK_IS_ENTRY(app->search)) {
    return GTK_ENTRY(app->search);
  }
  return NULL;
}

/** Keep the filter entry typable (not greyed, not read-only) after combo model rebuilds or list populate. */
static void da_file_view_search_ensure_editable(AppState *app) {
  if (app == NULL || app->search == NULL) {
    return;
  }
  gtk_widget_set_sensitive(app->search, TRUE);
  GtkEntry *en = da_file_view_search_get_entry(app);
  if (en != NULL) {
    gtk_widget_set_sensitive(GTK_WIDGET(en), TRUE);
    gtk_widget_set_can_focus(GTK_WIDGET(en), TRUE);
    gtk_editable_set_editable(GTK_EDITABLE(en), TRUE);
  }
}

static const gchar *da_file_view_search_get_text(const AppState *app) {
  GtkEntry *e = da_file_view_search_get_entry(app);
  if (e == NULL) {
    return "";
  }
  const gchar *t = gtk_entry_get_text(e);
  return t != NULL ? t : "";
}

static void da_search_combo_model_rebuild(AppState *app) {
  if (app == NULL || app->search == NULL || !GTK_IS_COMBO_BOX_TEXT(app->search) || app->search_history == NULL) {
    return;
  }
  GtkComboBoxText *cbt = GTK_COMBO_BOX_TEXT(app->search);
  g_signal_handlers_block_by_func(app->search, G_CALLBACK(on_search_combo_changed), app);
  gtk_combo_box_text_remove_all(cbt);
  for (guint i = 0; i < app->search_history->len; i++) {
    gtk_combo_box_text_append_text(cbt, (const gchar *)g_ptr_array_index(app->search_history, i));
  }
  gtk_combo_box_text_append_text(cbt, DA_SEARCH_HISTORY_CLEAR_ITEM);
  g_signal_handlers_unblock_by_func(app->search, G_CALLBACK(on_search_combo_changed), app);
  gtk_combo_box_set_active(GTK_COMBO_BOX(cbt), -1);
  da_file_view_search_ensure_editable(app);
}

static void da_search_history_persist_and_refresh(AppState *app) {
  if (app == NULL || app->search_history == NULL) {
    return;
  }
  const gchar **flat = g_new(const gchar *, app->search_history->len + 1u);
  for (guint i = 0; i < app->search_history->len; i++) {
    flat[i] = (const gchar *)g_ptr_array_index(app->search_history, i);
  }
  da_ini_search_history_save(flat, app->search_history->len);
  g_free(flat);
  da_search_combo_model_rebuild(app);
}

static void on_search_combo_changed(GtkComboBox *cb, gpointer user_data) {
  AppState *app = (AppState *)user_data;
  if (app == NULL || app->search == NULL || !GTK_IS_COMBO_BOX(cb)) {
    return;
  }
  gint active = gtk_combo_box_get_active(cb);
  if (active < 0) {
    return;
  }

  gchar *t = gtk_combo_box_text_get_active_text(GTK_COMBO_BOX_TEXT(cb));
  if (t == NULL) {
    return;
  }

  if (g_strcmp0(t, DA_SEARCH_HISTORY_CLEAR_ITEM) == 0) {
    g_free(t);
    while (app->search_history != NULL && app->search_history->len > 0u) {
      g_ptr_array_remove_index(app->search_history, 0u);
    }
    da_ini_search_history_save(NULL, 0);
    da_search_combo_model_rebuild(app);
    GtkEntry *en = da_file_view_search_get_entry(app);
    if (en != NULL) {
      gtk_entry_set_text(en, "");
    }
    gtk_combo_box_set_active(cb, -1);
    kill_timer(&app->timer_search);
    apply_search_filter(app);
    return;
  }

  GtkEntry *en = da_file_view_search_get_entry(app);
  if (en != NULL) {
    gtk_entry_set_text(en, t);
  }
  g_free(t);
  gtk_combo_box_set_active(cb, -1);
  kill_timer(&app->timer_search);
  apply_search_filter(app);
}

static void da_search_history_maybe_record(AppState *app) {
  if (app == NULL || app->search_history == NULL) {
    return;
  }
  const gchar *q = app->filter_text;
  if (q == NULL || q[0] == '\0') {
    return;
  }
  if (g_strcmp0(q, DA_SEARCH_HISTORY_CLEAR_ITEM) == 0) {
    return;
  }

  for (guint i = 0u; i < app->search_history->len;) {
    if (g_strcmp0((const gchar *)g_ptr_array_index(app->search_history, i), q) == 0) {
      g_ptr_array_remove_index(app->search_history, i);
      continue;
    }
    i++;
  }
  g_ptr_array_insert(app->search_history, 0, g_strdup(q));
  while (app->search_history->len > (guint)DA_SEARCH_HISTORY_MAX_ENTRIES) {
    g_ptr_array_remove_index(app->search_history, app->search_history->len - 1u);
  }
  da_search_history_persist_and_refresh(app);
}

void da_file_view_search_combo_init(AppState *app) {
  if (app == NULL || app->search == NULL || !GTK_IS_COMBO_BOX_TEXT(app->search)) {
    return;
  }
  if (app->search_history == NULL) {
    app->search_history = g_ptr_array_new_with_free_func(g_free);
    gsize n = 0;
    gchar **items = da_ini_search_history_load(&n);
    if (items != NULL) {
      for (gsize i = 0; i < n; i++) {
        if (items[i] != NULL && items[i][0] != '\0' &&
            g_strcmp0(items[i], DA_SEARCH_HISTORY_CLEAR_ITEM) != 0) {
          g_ptr_array_add(app->search_history, g_strdup(items[i]));
        }
      }
      g_strfreev(items);
    }
  }

  /* Do not bind entry text to the dropdown model; history picks copy text in on_search_combo_changed. */
  gtk_combo_box_set_entry_text_column(GTK_COMBO_BOX(app->search), -1);

  GtkEntry *en = da_file_view_search_get_entry(app);
  if (en != NULL) {
    gtk_entry_set_placeholder_text(en, "Filter (* ? wildcards)…");
    g_signal_connect(en, "changed", G_CALLBACK(on_search_changed), app);
    g_signal_connect(en, "activate", G_CALLBACK(on_search_entry_activate), app);
  }

  da_search_combo_model_rebuild(app);
  g_signal_connect(app->search, "changed", G_CALLBACK(on_search_combo_changed), app);
  da_file_view_search_ensure_editable(app);
}

void da_file_view_search_clear(AppState *app) {
  if (app == NULL) {
    return;
  }
  GtkEntry *en = da_file_view_search_get_entry(app);
  if (en != NULL) {
    gtk_entry_set_text(en, "");
  }
  if (app->search != NULL && GTK_IS_COMBO_BOX(app->search)) {
    gtk_combo_box_set_active(GTK_COMBO_BOX(app->search), -1);
  }
  kill_timer(&app->timer_search);
  apply_search_filter(app);
}

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
  da_ui_sync_file_menu(app);
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

static gboolean da_tv_paths_cmp_equal(const char *a, const char *b);

/* ---- Tree View selection → treemap sync ----------------------------------- */

static void on_tree_view_selection_changed(GtkTreeSelection *sel, gpointer user_data) {
  AppState *app = (AppState *)user_data;
  if (app == NULL) {
    return;
  }
  if (!scan_controller_is_tree_view_tab(app)) {
    da_ui_sync_file_menu(app);
    return;
  }
  scan_controller_update_status_left_from_tree_view_selection(app);

  if (!app->treemap_tree_sync_in_progress && app->treemap != NULL && TREEMAP_IS_WIDGET(app->treemap) &&
      app->scan != NULL) {
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
            if (v.nodes[ni].path != NULL && da_tv_paths_cmp_equal(v.nodes[ni].path, pstr)) {
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
  da_ui_sync_file_menu(app);
}

/* ---- treemap ↔ tree_view path matching (slashes, trailing seps, Windows case) -------- */

static gchar *da_tv_path_for_cmp_dup(const char *utf8) {
  gchar *s;
  gchar *p;
  if (utf8 == NULL) {
    return g_strdup("");
  }
  s = g_strdup(utf8);
#ifdef G_OS_WIN32
  for (p = s; *p != '\0'; p++) {
    if (*p == '\\') {
      *p = '/';
    }
  }
#endif
  gsize n = strlen(s);
  while (n > 0 && (s[n - 1] == '/' || s[n - 1] == '\\')) {
    s[--n] = '\0';
  }
  return s;
}

static gboolean da_tv_paths_cmp_equal(const char *a, const char *b) {
  gchar *na = da_tv_path_for_cmp_dup(a);
  gchar *nb = da_tv_path_for_cmp_dup(b);
  gboolean eq;
#ifdef G_OS_WIN32
  eq = (g_ascii_strcasecmp(na, nb) == 0);
#else
  eq = (strcmp(na, nb) == 0);
#endif
  g_free(na);
  g_free(nb);
  return eq;
}

/* ---- treemap → tree_view sync helper ------------------------------------- */

/**
 * BFS the tree_view_store from roots, expanding lazy rows until @a target_path row is found.
 * Does not clear the selection. Scrolls to the match when @a scroll is TRUE.
 */
static gboolean da_tv_bfs_select_one_path(AppState *app, GtkTreeView *tv, GtkTreeModel *model, GtkTreeSelection *sel,
                                          const char *target_path, gboolean scroll) {
  GtkTreeIter root_iter;
  gboolean found = FALSE;
  gchar *target_cmp = NULL;

  if (app == NULL || target_path == NULL || tv == NULL || model == NULL || sel == NULL) {
    return FALSE;
  }
  target_cmp = da_tv_path_for_cmp_dup(target_path);
  if (target_cmp == NULL) {
    return FALSE;
  }

  if (!gtk_tree_model_get_iter_first(model, &root_iter)) {
    g_free(target_cmp);
    return FALSE;
  }

  GQueue *queue = g_queue_new();
  {
    GtkTreeIter *copy = g_new(GtkTreeIter, 1);
    *copy = root_iter;
    g_queue_push_tail(queue, copy);
    GtkTreeIter sib = root_iter;
    while (gtk_tree_model_iter_next(model, &sib)) {
      GtkTreeIter *sc = g_new(GtkTreeIter, 1);
      *sc = sib;
      g_queue_push_tail(queue, sc);
    }
  }

  while (!g_queue_is_empty(queue) && !found) {
    GtkTreeIter *cur = (GtkTreeIter *)g_queue_pop_head(queue);
    gchar *pstr = NULL;
    gtk_tree_model_get(model, cur, DA_TV_COL_PATH, &pstr, -1);
    gchar *p_cmp = da_tv_path_for_cmp_dup(pstr);

    gboolean match = FALSE;
    if (p_cmp != NULL && p_cmp[0] != '\0') {
#ifdef G_OS_WIN32
      match = (g_ascii_strcasecmp(p_cmp, target_cmp) == 0);
#else
      match = (strcmp(p_cmp, target_cmp) == 0);
#endif
    }
    if (match) {
      gtk_tree_selection_select_iter(sel, cur);
      if (scroll) {
        GtkTreePath *tp = gtk_tree_model_get_path(model, cur);
        if (tp != NULL) {
          gtk_tree_view_scroll_to_cell(tv, tp, NULL, FALSE, 0.0f, 0.0f);
          gtk_tree_path_free(tp);
        }
      }
      found = TRUE;
    } else {
      gboolean is_ancestor = FALSE;
      if (p_cmp != NULL) {
        size_t clen = strlen(p_cmp);
        if (clen > 0) {
#ifdef G_OS_WIN32
          is_ancestor = (g_ascii_strncasecmp(p_cmp, target_cmp, (gint)clen) == 0) &&
                        (target_cmp[clen] == '/' || target_cmp[clen] == '\\' || target_cmp[clen] == '\0');
#else
          is_ancestor = (strncmp(p_cmp, target_cmp, clen) == 0) &&
                        (target_cmp[clen] == '/' || target_cmp[clen] == '\\' || target_cmp[clen] == '\0');
#endif
        }
      }
      if (is_ancestor && gtk_tree_model_iter_has_child(model, cur)) {
        GtkTreePath *tp = gtk_tree_model_get_path(model, cur);
        if (tp != NULL) {
          /* gtk_tree_view_expand_row may not emit row-expanded; populate lazily first. */
          da_tree_view_on_row_expanded(tv, cur, tp, app);
          gtk_tree_view_expand_row(tv, tp, FALSE);
          gtk_tree_path_free(tp);
        }
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
    g_free(p_cmp);
    g_free(pstr);
    g_free(cur);
  }

  while (!g_queue_is_empty(queue)) {
    g_free(g_queue_pop_head(queue));
  }
  g_queue_free(queue);
  g_free(target_cmp);
  return found;
}

/** Replace tree_view selection with rows matching every treemap-selected scan node. */
static void da_tv_sync_tree_from_treemap(AppState *app) {
  if (app == NULL || app->tree_view == NULL || app->treemap == NULL || !TREEMAP_IS_WIDGET(app->treemap) ||
      app->scan == NULL || app->tree_view_store == NULL) {
    return;
  }
  scan_results_view_t v = scan_get_results(app->scan);
  if (v.nodes == NULL) {
    return;
  }
  GArray *nids = g_array_new(FALSE, FALSE, sizeof(size_t));
  treemap_widget_append_selected_scan_indices(TREEMAP_WIDGET(app->treemap), nids);

  GtkTreeView *tv = GTK_TREE_VIEW(app->tree_view);
  GtkTreeModel *model = GTK_TREE_MODEL(app->tree_view_store);
  GtkTreeSelection *sel = gtk_tree_view_get_selection(tv);
  gtk_tree_selection_unselect_all(sel);

  if (nids->len == 0u) {
    g_array_free(nids, TRUE);
    return;
  }

  GPtrArray *paths = g_ptr_array_new();
  for (guint i = 0; i < nids->len; i++) {
    size_t nid = g_array_index(nids, size_t, i);
    if (nid >= v.count) {
      continue;
    }
    const char *p = v.nodes[nid].path;
    if (p == NULL || p[0] == '\0') {
      continue;
    }
    gboolean dup = FALSE;
    for (guint j = 0; j < paths->len; j++) {
      if (da_tv_paths_cmp_equal(p, (const char *)g_ptr_array_index(paths, j))) {
        dup = TRUE;
        break;
      }
    }
    if (!dup) {
      g_ptr_array_add(paths, (gpointer)p);
    }
  }

  gboolean scroll_once = TRUE;
  for (guint k = 0; k < paths->len; k++) {
    const char *path = (const char *)g_ptr_array_index(paths, k);
    gboolean ok = da_tv_bfs_select_one_path(app, tv, model, sel, path, scroll_once);
    if (ok) {
      scroll_once = FALSE;
    }
  }

  g_ptr_array_free(paths, TRUE);
  g_array_free(nids, TRUE);
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
  da_ui_sync_file_menu(app);
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
    /* Use zoom root when set, otherwise fall back to scan / CSV root. */
    if (app->treemap_zoom_root_utf8 != NULL && app->treemap_zoom_root_utf8[0] != '\0') {
      root_for_treemap = app->treemap_zoom_root_utf8;
    } else {
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

static void on_treemap_selected(GtkWidget *treemap, gint64 scan_index, gpointer user_data) {
  AppState *app = (AppState *)user_data;
  (void)treemap;
  if (app == NULL) {
    return;
  }
  /* stat_sel_val is owned exclusively by scan_controller_refresh_volume_labels;
   * do not write to it from treemap selection events. */
  if (app->treemap_tree_sync_in_progress) {
    da_ui_sync_file_menu(app);
    return;
  }
  app->treemap_tree_sync_in_progress = TRUE;
  if (scan_index < 0) {
    if (app->tree_view != NULL) {
      GtkTreeSelection *tsel = gtk_tree_view_get_selection(GTK_TREE_VIEW(app->tree_view));
      gtk_tree_selection_unselect_all(tsel);
    }
  } else {
    da_tv_sync_tree_from_treemap(app);
  }
  app->treemap_tree_sync_in_progress = FALSE;
  da_ui_sync_file_menu(app);
}

static const char *da_effective_treemap_root(AppState *app);

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
  if (scan_index == TREEMAP_SCAN_INDEX_FREE_SPACE) {
    /* Hovering over the free-space tile: show "Free Space: [<root>] <size>". */
    char free_sz[64];
    da_format_bytes(app->volume_free_bytes, free_sz, sizeof(free_sz));
    /* Use the most specific available root: zoom root → scan root → CSV derived root. */
    const char *root_lbl = da_effective_treemap_root(app);
    gchar *msg = g_strdup_printf("Free Space: [%s] %s",
                                  root_lbl != NULL ? root_lbl : "?",
                                  free_sz);
    gtk_label_set_text(GTK_LABEL(app->status_label_center), msg);
    g_free(msg);
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

/* ---- Treemap zoom --------------------------------------------------------- */

/**
 * Return the effective scan root for zoom navigation: the zoom root when set,
 * otherwise the scan root or CSV-derived root.
 */
static const char *da_effective_treemap_root(AppState *app) {
  if (app->treemap_zoom_root_utf8 != NULL && app->treemap_zoom_root_utf8[0] != '\0') {
    return app->treemap_zoom_root_utf8;
  }
  if (app->scan_root_utf8 != NULL && app->scan_root_utf8[0] != '\0') {
    return app->scan_root_utf8;
  }
  if (app->csv_import_active && app->csv_derived_root_utf8 != NULL) {
    return app->csv_derived_root_utf8;
  }
  return NULL;
}

/**
 * Zoom the treemap to the directory at @p path_utf8.  If @p path_utf8 already
 * equals or is above the effective root, the zoom is cleared instead.
 */
static void da_treemap_zoom_to_path(AppState *app, const char *path_utf8) {
  const char *base_root;
  if (path_utf8 == NULL || path_utf8[0] == '\0') {
    return;
  }
  /* If the target is the scan root or above it, reset the zoom. */
  base_root = app->scan_root_utf8;
  if (base_root == NULL) {
    base_root = app->csv_derived_root_utf8;
  }
  if (base_root != NULL && strcmp(path_utf8, base_root) == 0) {
    g_free(app->treemap_zoom_root_utf8);
    app->treemap_zoom_root_utf8 = NULL;
  } else {
    g_free(app->treemap_zoom_root_utf8);
    app->treemap_zoom_root_utf8 = g_strdup(path_utf8);
  }
  da_refresh_treemap(app);
  /* Zoom state changed — update zoom-in / zoom-out menu sensitivity. */
  da_ui_sync_file_menu(app);
}

static void on_treemap_zoom_in_cb(GtkWidget *treemap, gint64 scan_index, gpointer user_data) {
  AppState *app = (AppState *)user_data;
  (void)treemap;
  if (app == NULL || scan_index < 0 || app->scan == NULL) {
    return;
  }
  scan_results_view_t v = scan_get_results(app->scan);
  if (v.nodes == NULL || (size_t)scan_index >= v.count) {
    return;
  }
  const file_node_t *fn = &v.nodes[(size_t)scan_index];
  if (fn->path == NULL || fn->path[0] == '\0') {
    return;
  }
  uint32_t kind = fn->attributes & DISKATLAS_NODE_KIND_MASK;
  gchar *zoom_path;
  if (kind == DISKATLAS_NODE_KIND_DIR) {
    zoom_path = g_strdup(fn->path);
  } else {
    zoom_path = g_path_get_dirname(fn->path);
  }
  da_treemap_zoom_to_path(app, zoom_path);
  g_free(zoom_path);
}

void scan_controller_treemap_zoom_in(AppState *app) {
  if (app == NULL || app->treemap == NULL || !TREEMAP_IS_WIDGET(app->treemap)) {
    return;
  }
  if (app->scan == NULL) {
    return;
  }
  GArray *sel_nids = g_array_new(FALSE, FALSE, sizeof(size_t));
  treemap_widget_append_selected_scan_indices(TREEMAP_WIDGET(app->treemap), sel_nids);
  if (sel_nids->len == 0) {
    g_array_free(sel_nids, TRUE);
    return;
  }
  size_t first_nid = g_array_index(sel_nids, size_t, 0);
  g_array_free(sel_nids, TRUE);

  scan_results_view_t v = scan_get_results(app->scan);
  if (v.nodes == NULL || first_nid >= v.count) {
    return;
  }
  const file_node_t *fn = &v.nodes[first_nid];
  if (fn->path == NULL || fn->path[0] == '\0') {
    return;
  }
  uint32_t kind = fn->attributes & DISKATLAS_NODE_KIND_MASK;
  gchar *zoom_path;
  if (kind == DISKATLAS_NODE_KIND_DIR) {
    zoom_path = g_strdup(fn->path);
  } else {
    zoom_path = g_path_get_dirname(fn->path);
  }
  da_treemap_zoom_to_path(app, zoom_path);
  g_free(zoom_path);
}

void scan_controller_treemap_zoom_out(AppState *app) {
  const char *current_root;
  gchar *parent;
  const char *base_root;

  if (app == NULL) {
    return;
  }
  current_root = da_effective_treemap_root(app);
  if (current_root == NULL || current_root[0] == '\0') {
    return;
  }
  /* If we are already at the scan root there is nothing to zoom out to. */
  base_root = app->scan_root_utf8;
  if (base_root == NULL) {
    base_root = app->csv_derived_root_utf8;
  }
  if (base_root != NULL && strcmp(current_root, base_root) == 0) {
    return;
  }
  /* Move up one directory level. */
  parent = g_path_get_dirname(current_root);
  if (parent == NULL || strcmp(parent, current_root) == 0) {
    /* Already at filesystem root — clear zoom. */
    g_free(parent);
    g_free(app->treemap_zoom_root_utf8);
    app->treemap_zoom_root_utf8 = NULL;
    da_refresh_treemap(app);
    da_ui_sync_file_menu(app);
    return;
  }
  da_treemap_zoom_to_path(app, parent);
  g_free(parent);
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
  da_ui_cancel_pending_name_rename(app);
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

/** Updates the scan toolbar panel when an active scan reports progress (folder/file counts or dump bytes). */
static void scan_controller_repaint_scan_progress_panel(AppState *app) {
  if (app == NULL || app->scan == NULL) {
    return;
  }
  scan_progress_t pr = scan_get_progress(app->scan);
  if (pr.is_complete) {
    return;
  }
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
    const gchar *t = da_file_view_search_get_text(app);
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
    da_message_dialog_apply_layout(d);
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
    if (app->filter_active && !da_utf8_file_view_filter_matches(app, v.nodes[nid].path)) {
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
        da_message_dialog_apply_layout(d);
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

/** Classify all file nodes with MIME category colors using the active mime_db. No-op if not ready. */
static void da_mime_classify_scan_nodes(AppState *app) {
  if (app->mime_db == NULL || app->scan == NULL) return;
  size_t count = 0;
  file_node_t *nodes = scan_result_nodes_mutable(app->scan, &count);
  if (nodes != NULL && count > 0) {
    dm_mime_db_classify_nodes(app->mime_db, nodes, count);
    double shadow = app->treemap_style.gradient_strength *
                    (DM_TREEMAP_DEFAULT_SHADOW_STRENGTH / DM_TREEMAP_DEFAULT_GRADIENT_STRENGTH);
    dm_file_nodes_refresh_gradient_colors(nodes, count, app->treemap_style.gradient_strength, shadow);
  }
}

/** Reclassify current scan nodes with the active mime_db and queue a treemap redraw. */
void scan_controller_reclassify_mime(AppState *app) {
  da_mime_classify_scan_nodes(app);
  da_file_type_view_populate(app);
  if (app->treemap != NULL) {
    gtk_widget_queue_draw(app->treemap);
  }
}

static void begin_populate_list(AppState *app) {
  scan_results_view_t v = scan_get_results(app->scan);

  da_mime_classify_scan_nodes(app);

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
    da_file_type_view_clear(app);
    da_refresh_treemap(app);
    scan_controller_sync_file_view_status(app);
    da_fs_monitor_scan_phase_end(app);
    return;
  }

  app->populate_total = v.count;
  app->timer_fill = g_timeout_add(15, on_timer_fill_chunk, app);
}

static gboolean on_timer_fill_chunk(gpointer data) {
  AppState *app = (AppState *)data;
  if (app->scan == NULL || app->populate_total == 0) {
    kill_timer(&app->timer_fill);
    da_fs_monitor_scan_phase_end(app);
    return G_SOURCE_REMOVE;
  }

  scan_results_view_t v = scan_get_results(app->scan);

  if (app->master_indices == NULL) {
    if (v.nodes == NULL || v.count != app->populate_total) {
      kill_timer(&app->timer_fill);
      app->list_populated = TRUE;
      enable_scan_button(app, TRUE);
      scan_controller_sync_file_view_status(app);
      da_fs_monitor_scan_phase_end(app);
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
      da_message_dialog_apply_layout(d);
      gtk_dialog_run(GTK_DIALOG(d));
      gtk_widget_destroy(d);
      da_fs_monitor_scan_phase_end(app);
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
    da_fs_monitor_scan_phase_end(app);
    return G_SOURCE_REMOVE;
  }

  kill_timer(&app->timer_fill);

  app->list_populated = TRUE;
  da_file_view_search_ensure_editable(app);
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

  /* Folder tree builds asynchronously; defer fs monitor until `tv_build_done`. */
  if (!da_tree_view_populate(app)) {
    da_fs_monitor_scan_phase_end(app);
  }

  /* Populate file-type stats view. */
  da_file_type_view_populate(app);

  /* Populate flat file-view list — instantaneous, no timer loop needed. */
  const gchar *peek = da_file_view_search_get_text(app);
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
    da_message_dialog_apply_layout(d);
    gtk_dialog_run(GTK_DIALOG(d));
    gtk_widget_destroy(d);
    g_free(dest_owned);
    scan_controller_refresh_volume_labels(app);
    enable_scan_button(app, TRUE);
    da_fs_monitor_scan_phase_end(app);
    return;
  }
  panel_scan_set_text(app, "MFT data dump complete.");
  scan_progress_set_full(app);
  {
    gchar *msg = g_strdup_printf("MFT data dumped to file %s", dest_owned);
    GtkWidget *d = gtk_message_dialog_new(GTK_WINDOW(app->window), GTK_DIALOG_MODAL, GTK_MESSAGE_INFO,
                                          GTK_BUTTONS_OK, "%s", msg);
    g_free(msg);
    da_message_dialog_apply_layout(d);
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
    da_ui_sync_file_menu(app);
    da_fs_monitor_scan_phase_end(app);
    return G_SOURCE_REMOVE;
  }
  scan_progress_t pr = scan_get_progress(app->scan);

  if (!pr.is_complete) {
    scan_controller_repaint_scan_progress_panel(app);
    return G_SOURCE_CONTINUE;
  }

  kill_timer(&app->timer_scan);
  da_ui_sync_file_menu(app);
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

static void on_search_entry_activate(GtkEntry *entry, gpointer user_data) {
  (void)entry;
  AppState *app = (AppState *)user_data;
  if (app == NULL) {
    return;
  }
  /* Enter: apply current text immediately, then add to history (not on every keystroke). */
  kill_timer(&app->timer_search);
  apply_search_filter(app);
  da_search_history_maybe_record(app);
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

  /* Stop monitor and block restarts until populate finishes (also blocks settings Apply). */
  da_fs_monitor_scan_phase_begin(app);

  if (!app->mft_dump_internal_scan) {
    mft_dump_flow_clear(app);
  }

  app->csv_import_active = FALSE;
  g_free(app->csv_import_path);
  app->csv_import_path = NULL;
  g_free(app->csv_derived_root_utf8);
  app->csv_derived_root_utf8 = NULL;
  app->import_snapshot_is_raw_mft = FALSE;
  /* Clear any zoom state when a new scan starts. */
  g_free(app->treemap_zoom_root_utf8);
  app->treemap_zoom_root_utf8 = NULL;

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

  if (app->deleted_path_set != NULL) {
    g_hash_table_destroy(app->deleted_path_set);
    app->deleted_path_set = NULL;
  }

  if (app->scan != NULL) {
    if (app->tv_held_scan == app->scan) {
      /* Background tree-view build is using this scan; the GTask callback will free it. */
      app->scan = NULL;
    } else {
      scan_result_free(app->scan);
      app->scan = NULL;
    }
  }
  da_ui_sync_file_menu(app);

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
    da_message_dialog_apply_layout(d);
    gtk_dialog_run(GTK_DIALOG(d));
    gtk_widget_destroy(d);
    enable_scan_button(app, TRUE);
    scan_progress_reset_idle(app);
    panel_scan_set_text(app, "Could not start scan.");
    da_ui_sync_file_menu(app);
    da_fs_monitor_scan_phase_end(app);
    return;
  }

  da_ui_sync_file_menu(app);
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
  /* Avoid racing a new scan against the folder-tree GTask (uses tv_held_scan / scan nodes). */
  if (app->tv_build_worker_pending) {
    panel_scan_set_text(app, "Finishing folder tree…");
    return;
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
    da_message_dialog_apply_layout(d);
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

static void on_match_scope_toggled(GtkToggleButton *btn, gpointer user_data) {
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
  app->volume_free_bytes  = free_b;
  app->volume_pct_denominator_bytes = (used_b > 0u) ? used_b : tot;
  if (app->flat_list_model != NULL) {
    flat_list_model_invalidate(app->flat_list_model);
  }
  /* Propagate updated free-space info to the treemap widget. */
  if (app->treemap != NULL && TREEMAP_IS_WIDGET(app->treemap)) {
    treemap_widget_set_free_space(TREEMAP_WIDGET(app->treemap),
                                  app->interface_treemap_show_free_space,
                                  free_b, (used_b > 0u) ? used_b : tot,
                                  app->scan_root_utf8);
  }
  char a[64];
  char use_line[160];
  char free_line[160];
  da_format_bytes(tot, a, sizeof(a));
  da_format_bytes_with_pct(used_b, tot, use_line, sizeof(use_line));
  da_format_bytes_with_pct(free_b, tot, free_line, sizeof(free_line));
  if (app->stat_sel_val) {
    gchar *sel_lbl = da_volume_status_selected_label(app->scan_root_utf8);
    gtk_label_set_text(GTK_LABEL(app->stat_sel_val), sel_lbl != NULL ? sel_lbl : app->scan_root_utf8);
    g_free(sel_lbl);
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

/**
 * Reapply the active byte-formatting preference across lists, tree view, volume/status labels,
 * treemap, and the scan progress panel (when a scan is running).
 */
void scan_controller_refresh_size_display_format(AppState *app) {
  if (app == NULL) {
    return;
  }
  da_format_bytes_set_decimal_places(app->size_decimal_places);
  da_format_bytes_set_display_format(app->interface_size_display_format);
  if (app->flat_list_model != NULL) {
    flat_list_model_invalidate(app->flat_list_model);
  }
  da_tree_view_refresh_size_columns(app);
  scan_controller_refresh_volume_labels(app);
  scan_controller_repaint_scan_progress_panel(app);
  if (app->treemap != NULL) {
    gtk_widget_queue_draw(app->treemap);
  }
  if (app->file_type_tree != NULL) {
    da_file_type_view_populate(app);
  }
}

void scan_controller_set_size_display_format(AppState *app, gint format) {
  if (app == NULL) {
    return;
  }
  if (format < (gint)DA_SIZE_DISPLAY_DYNAMIC || format > (gint)DA_SIZE_DISPLAY_TB) {
    format = DA_SIZE_DISPLAY_DYNAMIC;
  }
  app->interface_size_display_format = format;
  da_ini_save_interface(app);
  scan_controller_refresh_size_display_format(app);
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
  if (app->match_filename_only_radio != NULL) {
    g_signal_connect(app->match_filename_only_radio, "toggled", G_CALLBACK(on_match_scope_toggled), app);
  }
  if (app->match_entire_path_radio != NULL) {
    g_signal_connect(app->match_entire_path_radio, "toggled", G_CALLBACK(on_match_scope_toggled), app);
  }
  if (app->show_folders_check != NULL) {
    g_signal_connect(app->show_folders_check, "toggled", G_CALLBACK(on_show_folders_toggled), app);
  }
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
    treemap_widget_set_zoom_callback(TREEMAP_WIDGET(app->treemap), on_treemap_zoom_in_cb, app);
    /* Apply initial label and show-labels state from loaded preferences. */
    treemap_widget_set_show_labels(TREEMAP_WIDGET(app->treemap), app->interface_treemap_show_labels);
  }

  /* Set up outbound drag-and-drop on both tree views (column 0 only). */
  da_drag_drop_setup(app);
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

  /* Stop monitor; imported scans reflect a snapshot, not live state. */
  da_fs_monitor_scan_phase_begin(app);

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

  if (app->deleted_path_set != NULL) {
    g_hash_table_destroy(app->deleted_path_set);
    app->deleted_path_set = NULL;
  }

  if (app->scan != NULL) {
    if (app->tv_held_scan == app->scan) {
      app->scan = NULL;
    } else {
      scan_result_free(app->scan);
    }
    app->scan = NULL;
  }

  app->scan = new_scan;

  /* Clear zoom root on every import so the new root is shown in full. */
  g_free(app->treemap_zoom_root_utf8);
  app->treemap_zoom_root_utf8 = NULL;

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
  da_ui_sync_file_menu(app);
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

static void da_explicit_sel_nids_append_unique(GArray *nids, size_t nid) {
  if (nid == SIZE_MAX || nids == NULL) {
    return;
  }
  for (guint i = 0; i < nids->len; i++) {
    if (g_array_index(nids, size_t, i) == nid) {
      return;
    }
  }
  g_array_append_val(nids, nid);
}

/**
 * Collects scan node ids for the current explicit UI selection only (no fallback to whole scan).
 * File View: treemap selection wins if non-empty, else file list rows. Tree View: selected rows only.
 */
static gboolean da_collect_explicit_selected_scan_nids(AppState *app, const scan_results_view_t *v, GArray *out_nids) {
  if (app == NULL || v == NULL || v->nodes == NULL || out_nids == NULL) {
    return FALSE;
  }

  if (scan_controller_is_file_view_tab(app)) {
    /* File View tab: only the file-view list widget carries the selection. */
    if (app->tree != NULL) {
      GtkTreeSelection *sel = gtk_tree_view_get_selection(GTK_TREE_VIEW(app->tree));
      GtkTreeModel *model = gtk_tree_view_get_model(GTK_TREE_VIEW(app->tree));
      GList *plist = gtk_tree_selection_get_selected_rows(sel, &model);
      if (plist != NULL) {
        for (GList *l = plist; l != NULL; l = l->next) {
          GtkTreeIter it;
          if (!gtk_tree_model_get_iter(model, &it, (GtkTreePath *)l->data)) {
            continue;
          }
          gint64 lp = 0;
          gtk_tree_model_get(model, &it, DA_COL_LP, &lp, -1);
          size_t nid = SIZE_MAX;
          if (!da_tree_lp_to_scan_nid(app, lp, &nid) || nid == SIZE_MAX || nid >= v->count) {
            continue;
          }
          da_explicit_sel_nids_append_unique(out_nids, nid);
        }
        g_list_free_full(plist, (GDestroyNotify)gtk_tree_path_free);
      }
    }
  } else if (scan_controller_is_tree_view_tab(app)) {
    /* Tree View tab: the treemap (lower pane) takes priority; fall back to the
     * tree-view list widget when the treemap carries no selection. */
    gboolean treemap_used = FALSE;
    if (app->treemap != NULL && TREEMAP_IS_WIDGET(app->treemap)) {
      GArray *tn = g_array_new(FALSE, FALSE, sizeof(size_t));
      treemap_widget_append_selected_scan_indices(TREEMAP_WIDGET(app->treemap), tn);
      if (tn->len > 0) {
        treemap_used = TRUE;
        for (guint i = 0; i < tn->len; i++) {
          size_t nid = g_array_index(tn, size_t, i);
          if (nid < v->count) {
            da_explicit_sel_nids_append_unique(out_nids, nid);
          }
        }
      }
      g_array_free(tn, TRUE);
    }
    if (!treemap_used && app->tree_view != NULL) {
      GtkTreeSelection *sel = gtk_tree_view_get_selection(GTK_TREE_VIEW(app->tree_view));
      GtkTreeModel *model = gtk_tree_view_get_model(GTK_TREE_VIEW(app->tree_view));
      GList *plist = gtk_tree_selection_get_selected_rows(sel, &model);
      if (plist != NULL) {
        for (GList *l = plist; l != NULL; l = l->next) {
          GtkTreeIter it;
          if (!gtk_tree_model_get_iter(model, &it, (GtkTreePath *)l->data)) {
            continue;
          }
          gint64 idx_id = DA_TV_LP_PLACEHOLDER;
          gtk_tree_model_get(model, &it, DA_TV_COL_IDX_ID, &idx_id, -1);
          if (idx_id == DA_TV_LP_PLACEHOLDER) {
            continue;
          }
          gchar *pstr = NULL;
          gtk_tree_model_get(model, &it, DA_TV_COL_PATH, &pstr, -1);
          size_t nid = da_find_scan_nid_for_utf8_path(v, pstr);
          g_free(pstr);
          if (nid != SIZE_MAX && nid < v->count) {
            da_explicit_sel_nids_append_unique(out_nids, nid);
          }
        }
        g_list_free_full(plist, (GDestroyNotify)gtk_tree_path_free);
      }
    }
  }

  return out_nids->len > 0u;
}

static gchar *da_containing_directory_utf8(const file_node_t *node) {
  if (node == NULL) {
    return NULL;
  }
  const char *p = node->path;
  if (p == NULL || p[0] == '\0') {
    return NULL;
  }
  uint32_t kind = node->attributes & DISKATLAS_NODE_KIND_MASK;
  if (kind == DISKATLAS_NODE_KIND_DIR) {
    return g_strdup(p);
  }
  return g_path_get_dirname(p);
}

static gboolean da_dir_list_contains(GPtrArray *dirs, const char *candidate) {
  if (candidate == NULL) {
    return TRUE;
  }
  for (guint i = 0; i < dirs->len; i++) {
    const char *existing = (const char *)g_ptr_array_index(dirs, i);
    if (existing == NULL) {
      continue;
    }
#ifdef G_OS_WIN32
    if (g_ascii_strcasecmp(existing, candidate) == 0) {
      return TRUE;
    }
#else
    if (strcmp(existing, candidate) == 0) {
      return TRUE;
    }
#endif
  }
  return FALSE;
}

static void da_spawn_explore_folder(const char *dir_utf8) {
  if (dir_utf8 == NULL || dir_utf8[0] == '\0') {
    return;
  }
  GError *err = NULL;
#if defined(G_OS_WIN32)
  const gchar *argv[] = {"explorer.exe", dir_utf8, NULL};
  g_spawn_async(NULL, (gchar **)argv, NULL, G_SPAWN_SEARCH_PATH, NULL, NULL, NULL, &err);
#elif defined(G_OS_DARWIN)
  const gchar *argv[] = {"/usr/bin/open", dir_utf8, NULL};
  g_spawn_async(NULL, (gchar **)argv, NULL, (GSpawnFlags)0, NULL, NULL, NULL, &err);
#else
  const gchar *argv[] = {"xdg-open", dir_utf8, NULL};
  g_spawn_async(NULL, (gchar **)argv, NULL, G_SPAWN_SEARCH_PATH, NULL, NULL, NULL, &err);
#endif
  if (err != NULL) {
    g_warning("Explore folder: %s", err->message);
    g_clear_error(&err);
  }
}

void scan_controller_launch_path_with_default_app(const gchar *path_utf8) {
  if (path_utf8 == NULL || path_utf8[0] == '\0') {
    return;
  }
  GFile *file = g_file_new_for_path(path_utf8);
  gchar *uri = g_file_get_uri(file);
  g_object_unref(file);
  if (uri == NULL) {
    return;
  }
  GError *err = NULL;
  if (!g_app_info_launch_default_for_uri(uri, NULL, &err)) {
    g_warning("Open with default app: %s", err->message);
    g_clear_error(&err);
  }
  g_free(uri);
}

static void da_spawn_terminal_in_dir(const char *dir_utf8) {
  if (dir_utf8 == NULL || dir_utf8[0] == '\0') {
    return;
  }
  GError *err = NULL;
#if defined(G_OS_WIN32)
  /* `start` opens a new console window; plain `cmd /k` can attach to the GUI app and stay hidden. */
  gchar *argv[] = {"cmd.exe", "/c", "start", "", "/D", (gchar *)dir_utf8, "cmd.exe", "/k", NULL};
  g_spawn_async(NULL, argv, NULL,
                (GSpawnFlags)(G_SPAWN_SEARCH_PATH | G_SPAWN_STDOUT_TO_DEV_NULL | G_SPAWN_STDERR_TO_DEV_NULL), NULL,
                NULL, NULL, &err);
#elif defined(G_OS_DARWIN)
  /* `-n` always starts a new Terminal instance instead of reusing a window/tab. */
  const gchar *argv[] = {"/usr/bin/open", "-na", "Terminal", ".", NULL};
  g_spawn_async(dir_utf8, (gchar **)argv, NULL, (GSpawnFlags)0, NULL, NULL, NULL, &err);
#else
  gchar *prog = g_find_program_in_path("gnome-terminal");
  if (prog != NULL) {
    gchar *argv[] = {prog, "--window", "--working-directory", (gchar *)dir_utf8, NULL};
    g_spawn_async(NULL, argv, NULL, G_SPAWN_SEARCH_PATH, NULL, NULL, NULL, &err);
    g_free(prog);
    if (err == NULL) {
      return;
    }
    g_clear_error(&err);
  }
  prog = g_find_program_in_path("konsole");
  if (prog != NULL) {
    gchar *argv[] = {prog, "--separate", "--workdir", (gchar *)dir_utf8, NULL};
    g_spawn_async(NULL, argv, NULL, G_SPAWN_SEARCH_PATH, NULL, NULL, NULL, &err);
    g_free(prog);
    if (err == NULL) {
      return;
    }
    g_clear_error(&err);
  }
  prog = g_find_program_in_path("xfce4-terminal");
  if (prog != NULL) {
    gchar *argv[] = {prog, "--disable-server", "--working-directory", (gchar *)dir_utf8, NULL};
    g_spawn_async(NULL, argv, NULL, G_SPAWN_SEARCH_PATH, NULL, NULL, NULL, &err);
    g_free(prog);
    if (err == NULL) {
      return;
    }
    g_clear_error(&err);
  }
  prog = g_find_program_in_path("xterm");
  if (prog != NULL) {
    gchar *argv[] = {prog, "-e", "/bin/bash", NULL};
    g_spawn_async(dir_utf8, argv, NULL, G_SPAWN_SEARCH_PATH, NULL, NULL, NULL, &err);
    g_free(prog);
    if (err == NULL) {
      return;
    }
    g_clear_error(&err);
  }
  prog = g_find_program_in_path("x-terminal-emulator");
  if (prog != NULL) {
    gchar *argv[] = {prog, "-e", "/bin/bash", NULL};
    g_spawn_async(dir_utf8, argv, NULL, G_SPAWN_SEARCH_PATH, NULL, NULL, NULL, &err);
    g_free(prog);
  }
#endif
  if (err != NULL) {
    g_warning("Open terminal: %s", err->message);
    g_clear_error(&err);
  }
}

gboolean scan_controller_is_tree_view_tab_active(AppState *app) {
  return scan_controller_is_tree_view_tab(app);
}

gboolean scan_controller_file_menu_selection_commands_sensitive(AppState *app) {
  if (app == NULL || app->scan == NULL) {
    return FALSE;
  }
  scan_progress_t pr = scan_get_progress(app->scan);
  if (!pr.is_complete) {
    return FALSE;
  }
  scan_results_view_t v = scan_get_results(app->scan);
  if (v.nodes == NULL) {
    return FALSE;
  }
  GArray *nids = g_array_new(FALSE, FALSE, sizeof(size_t));
  gboolean ok = da_collect_explicit_selected_scan_nids(app, &v, nids);
  g_array_free(nids, TRUE);
  return ok;
}

void scan_controller_explore_selected_folders(AppState *app) {
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
  GArray *nids = g_array_new(FALSE, FALSE, sizeof(size_t));
  if (!da_collect_explicit_selected_scan_nids(app, &v, nids)) {
    g_array_free(nids, TRUE);
    return;
  }
  GPtrArray *dirs = g_ptr_array_new_with_free_func(g_free);
  for (guint i = 0; i < nids->len; i++) {
    size_t nid = g_array_index(nids, size_t, i);
    if (nid >= v.count) {
      continue;
    }
    gchar *d = da_containing_directory_utf8(&v.nodes[nid]);
    if (d != NULL && d[0] != '\0' && !da_dir_list_contains(dirs, d)) {
      g_ptr_array_add(dirs, d);
    } else {
      g_free(d);
    }
  }
  g_array_free(nids, TRUE);
  for (guint j = 0; j < dirs->len; j++) {
    da_spawn_explore_folder((const char *)g_ptr_array_index(dirs, j));
  }
  g_ptr_array_free(dirs, TRUE);
}

void scan_controller_open_terminal_here(AppState *app) {
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
  GArray *nids = g_array_new(FALSE, FALSE, sizeof(size_t));
  if (!da_collect_explicit_selected_scan_nids(app, &v, nids)) {
    g_array_free(nids, TRUE);
    return;
  }
  GPtrArray *dirs = g_ptr_array_new_with_free_func(g_free);
  for (guint i = 0; i < nids->len; i++) {
    size_t nid = g_array_index(nids, size_t, i);
    if (nid >= v.count) {
      continue;
    }
    gchar *d = da_containing_directory_utf8(&v.nodes[nid]);
    if (d != NULL && d[0] != '\0' && !da_dir_list_contains(dirs, d)) {
      g_ptr_array_add(dirs, d);
    } else {
      g_free(d);
    }
  }
  g_array_free(nids, TRUE);
  for (guint j = 0; j < dirs->len; j++) {
    da_spawn_terminal_in_dir((const char *)g_ptr_array_index(dirs, j));
  }
  g_ptr_array_free(dirs, TRUE);
}

void scan_controller_copy_selected_paths_to_clipboard(AppState *app) {
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
  GArray *nids = g_array_new(FALSE, FALSE, sizeof(size_t));
  if (!da_collect_explicit_selected_scan_nids(app, &v, nids)) {
    g_array_free(nids, TRUE);
    return;
  }
  GPtrArray *lines = g_ptr_array_new_with_free_func(g_free);
  for (guint i = 0; i < nids->len; i++) {
    size_t nid = g_array_index(nids, size_t, i);
    if (nid >= v.count) {
      continue;
    }
    const char *p = v.nodes[nid].path;
    if (p != NULL && p[0] != '\0') {
      g_ptr_array_add(lines, g_strdup(p));
    }
  }
  g_array_free(nids, TRUE);
  if (lines->len == 0u) {
    g_ptr_array_free(lines, TRUE);
    return;
  }
  GString *gs = g_string_new(NULL);
  for (guint k = 0; k < lines->len; k++) {
    if (k > 0u) {
      g_string_append_c(gs, '\n');
    }
    g_string_append(gs, (const gchar *)g_ptr_array_index(lines, k));
  }
  GtkClipboard *cb = gtk_widget_get_clipboard(app->window, GDK_SELECTION_CLIPBOARD);
  gtk_clipboard_set_text(cb, gs->str, -1);
  g_string_free(gs, TRUE);
  g_ptr_array_free(lines, TRUE);
}

GPtrArray *scan_controller_collect_selected_utf8_paths(AppState *app) {
  if (app == NULL || app->scan == NULL) {
    return NULL;
  }
  scan_progress_t pr = scan_get_progress(app->scan);
  if (!pr.is_complete) {
    return NULL;
  }
  scan_results_view_t v = scan_get_results(app->scan);
  if (v.nodes == NULL) {
    return NULL;
  }
  GArray *nids = g_array_new(FALSE, FALSE, sizeof(size_t));
  if (!da_collect_explicit_selected_scan_nids(app, &v, nids) || nids->len == 0) {
    g_array_free(nids, TRUE);
    return NULL;
  }
  GPtrArray *paths = g_ptr_array_new_with_free_func(g_free);
  for (guint i = 0; i < nids->len; i++) {
    size_t nid = g_array_index(nids, size_t, i);
    if (nid >= v.count) {
      continue;
    }
    const char *p = v.nodes[nid].path;
    if (p != NULL && p[0] != '\0') {
      g_ptr_array_add(paths, g_strdup(p));
    }
  }
  g_array_free(nids, TRUE);
  if (paths->len == 0) {
    g_ptr_array_free(paths, TRUE);
    return NULL;
  }
  return paths;
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
  if (app != NULL && app->deleted_path_set != NULL) {
    g_hash_table_destroy(app->deleted_path_set);
    app->deleted_path_set = NULL;
  }
}

/* =========================================================================
 * File operations: Copy, Cut, Delete to Trash, Permanently Delete
 * ========================================================================= */

/* Returns TRUE if node path equals dir_path or is a direct descendant. */
static gboolean da_path_is_under_dir(const char *path, const char *dir_path) {
  if (path == NULL || dir_path == NULL || dir_path[0] == '\0') {
    return FALSE;
  }
  size_t dir_len = strlen(dir_path);

  /* Strip trailing separator from dir_path for the comparison. */
  size_t cmp_len = dir_len;
  char last = dir_path[dir_len - 1];
  if (last == '/' || last == '\\') {
    cmp_len = dir_len - 1;
    if (cmp_len == 0) {
      return FALSE;
    }
  }

#if defined(G_OS_WIN32)
  if (g_ascii_strncasecmp(path, dir_path, cmp_len) != 0) {
    return FALSE;
  }
#else
  if (strncmp(path, dir_path, cmp_len) != 0) {
    return FALSE;
  }
#endif

  char next = path[cmp_len];
  /* Exact match or child (next char is a separator). */
  return (next == '\0' || next == '/' || next == '\\');
}

/**
 * Expand deleted paths into the set: for each path that is a directory,
 * scan all nodes and add descendant paths.  Refresh both views.
 *
 * @param app          AppState.
 * @param paths        UTF-8 paths that were just deleted.
 * @param count        Number of paths.
 * @param node_kinds   Parallel array of DISKATLAS_NODE_KIND_* for each path
 *                     (used to know which entries are directories).
 */
static void da_mark_deleted_paths(AppState *app, const char **paths, size_t count,
                                   const uint32_t *node_kinds) {
  if (app == NULL || paths == NULL || count == 0) {
    return;
  }

  if (app->deleted_path_set == NULL) {
    app->deleted_path_set = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
    if (app->deleted_path_set == NULL) {
      return;
    }
  }

  /* Add each deleted path directly. */
  for (size_t i = 0; i < count; i++) {
    if (paths[i] != NULL && paths[i][0] != '\0') {
      if (!g_hash_table_contains(app->deleted_path_set, paths[i])) {
        g_hash_table_insert(app->deleted_path_set, g_strdup(paths[i]), NULL);
      }
    }
  }

  /* For directory paths, also mark all scan nodes under them. */
  gboolean have_dirs = FALSE;
  if (node_kinds != NULL) {
    for (size_t i = 0; i < count; i++) {
      if ((node_kinds[i] & DISKATLAS_NODE_KIND_MASK) == DISKATLAS_NODE_KIND_DIR) {
        have_dirs = TRUE;
        break;
      }
    }
  }

  if (have_dirs && app->scan != NULL) {
    scan_results_view_t v = scan_get_results(app->scan);
    if (v.nodes != NULL && v.count > 0) {
      for (size_t ni = 0; ni < v.count; ni++) {
        const char *np = v.nodes[ni].path;
        if (np == NULL || np[0] == '\0') {
          continue;
        }
        if (g_hash_table_contains(app->deleted_path_set, np)) {
          continue;
        }
        for (size_t di = 0; di < count; di++) {
          if (node_kinds == NULL) {
            continue;
          }
          if ((node_kinds[di] & DISKATLAS_NODE_KIND_MASK) != DISKATLAS_NODE_KIND_DIR) {
            continue;
          }
          if (paths[di] == NULL || paths[di][0] == '\0') {
            continue;
          }
          if (da_path_is_under_dir(np, paths[di])) {
            g_hash_table_insert(app->deleted_path_set, g_strdup(np), NULL);
            break;
          }
        }
      }
    }
  }

  /* Refresh both views. */
  if (app->flat_list_model != NULL) {
    flat_list_model_invalidate(app->flat_list_model);
  }
  if (app->tree_view != NULL) {
    gtk_widget_queue_draw(app->tree_view);
  }
  da_ui_sync_file_menu(app);
}

void scan_controller_mark_path_deleted(AppState *app, const gchar *path_utf8) {
  if (app == NULL || path_utf8 == NULL || path_utf8[0] == '\0') {
    return;
  }

  /* Determine whether the path is a directory by searching the scan nodes,
   * so that da_mark_deleted_paths can recursively mark all children.
   *
   * Use the scan node's own path pointer as the canonical key so that the
   * string inserted into deleted_path_set is byte-for-byte identical to what
   * cell-data functions look up (v.nodes[nid].path / DA_TV_COL_PATH).
   * This avoids mismatches caused by path-separator normalisation differences
   * between g_file_get_path() and the scan engine on Windows. */
  uint32_t kind           = DISKATLAS_NODE_KIND_FILE;
  const char *canonical   = path_utf8;   /* fallback: use as-is */

  if (app->scan != NULL) {
    scan_results_view_t v = scan_get_results(app->scan);
    for (size_t i = 0; i < v.count; i++) {
      if (v.nodes[i].path == NULL) {
        continue;
      }
#ifdef G_OS_WIN32
      /* Windows paths are case-insensitive.  The scan engine stores paths with
       * backslashes while the Win32 monitor produces forward slashes, so we
       * normalise both separators to '/' before comparing. */
      gboolean match = FALSE;
      { const char *_np = v.nodes[i].path, *_ip = path_utf8;
        for (;;) {
          char _nc = (*_np == '\\') ? '/' : *_np;
          char _ic = (*_ip == '\\') ? '/' : *_ip;
          if (g_ascii_tolower(_nc) != g_ascii_tolower(_ic)) break;
          if (_nc == '\0') { match = TRUE; break; }
          _np++; _ip++;
        }
      }
#else
      gboolean match = (strcmp(v.nodes[i].path, path_utf8) == 0);
#endif
      if (match) {
        kind      = v.nodes[i].attributes & DISKATLAS_NODE_KIND_MASK;
        canonical = v.nodes[i].path;
        break;
      }
    }
  }

  da_mark_deleted_paths(app, &canonical, 1, &kind);
}

/* Shared boilerplate: verify app state, get results, collect selected nids.
 * Returns a GArray* of size_t nids that must be freed by the caller,
 * or NULL if not actionable. */
static GArray *da_get_selected_nids_for_fileop(AppState *app) {
  if (app == NULL || app->window == NULL || app->scan == NULL) {
    return NULL;
  }
  scan_progress_t pr = scan_get_progress(app->scan);
  if (!pr.is_complete) {
    return NULL;
  }
  scan_results_view_t v = scan_get_results(app->scan);
  if (v.nodes == NULL) {
    return NULL;
  }
  GArray *nids = g_array_new(FALSE, FALSE, sizeof(size_t));
  if (!da_collect_explicit_selected_scan_nids(app, &v, nids)) {
    g_array_free(nids, TRUE);
    return NULL;
  }
  return nids;
}

void scan_controller_copy_files(AppState *app) {
  GArray *nids = da_get_selected_nids_for_fileop(app);
  if (nids == NULL) {
    return;
  }
  scan_results_view_t v = scan_get_results(app->scan);
  GPtrArray *path_arr = g_ptr_array_new();
  for (guint i = 0; i < nids->len; i++) {
    size_t nid = g_array_index(nids, size_t, i);
    if (nid < v.count && v.nodes[nid].path != NULL && v.nodes[nid].path[0] != '\0') {
      g_ptr_array_add(path_arr, (gpointer)v.nodes[nid].path);
    }
  }
  g_array_free(nids, TRUE);
  if (path_arr->len > 0) {
    da_file_op_copy_to_clipboard(app->window, (const char **)path_arr->pdata, path_arr->len);
  }
  g_ptr_array_free(path_arr, TRUE);
}

void scan_controller_cut_files(AppState *app) {
  GArray *nids = da_get_selected_nids_for_fileop(app);
  if (nids == NULL) {
    return;
  }
  scan_results_view_t v = scan_get_results(app->scan);
  GPtrArray *path_arr = g_ptr_array_new();
  for (guint i = 0; i < nids->len; i++) {
    size_t nid = g_array_index(nids, size_t, i);
    if (nid < v.count && v.nodes[nid].path != NULL && v.nodes[nid].path[0] != '\0') {
      g_ptr_array_add(path_arr, (gpointer)v.nodes[nid].path);
    }
  }
  g_array_free(nids, TRUE);
  if (path_arr->len > 0) {
    da_file_op_cut_to_clipboard(app->window, (const char **)path_arr->pdata, path_arr->len);
  }
  g_ptr_array_free(path_arr, TRUE);
}

void scan_controller_delete_to_trash(AppState *app) {
  GArray *nids = da_get_selected_nids_for_fileop(app);
  if (nids == NULL) {
    return;
  }
  scan_results_view_t v = scan_get_results(app->scan);
  GPtrArray *path_arr  = g_ptr_array_new();
  GArray    *kind_arr  = g_array_new(FALSE, FALSE, sizeof(uint32_t));
  for (guint i = 0; i < nids->len; i++) {
    size_t nid = g_array_index(nids, size_t, i);
    if (nid >= v.count || v.nodes[nid].path == NULL || v.nodes[nid].path[0] == '\0') {
      continue;
    }
    g_ptr_array_add(path_arr, (gpointer)v.nodes[nid].path);
    uint32_t k = v.nodes[nid].attributes & DISKATLAS_NODE_KIND_MASK;
    g_array_append_val(kind_arr, k);
  }
  g_array_free(nids, TRUE);

  if (path_arr->len == 0) {
    g_ptr_array_free(path_arr, TRUE);
    g_array_free(kind_arr, TRUE);
    return;
  }

  GError *err = NULL;
  gboolean ok = da_file_op_trash((const char **)path_arr->pdata, path_arr->len, &err);

  if (ok) {
    da_mark_deleted_paths(app, (const char **)path_arr->pdata, path_arr->len,
                          (const uint32_t *)kind_arr->data);
  } else {
    if (err != NULL) {
      GtkWidget *d = gtk_message_dialog_new(GTK_WINDOW(app->window), GTK_DIALOG_MODAL,
                                            GTK_MESSAGE_ERROR, GTK_BUTTONS_OK,
                                            "Could not move to trash: %s", err->message);
      da_message_dialog_apply_layout(d);
      gtk_dialog_run(GTK_DIALOG(d));
      gtk_widget_destroy(d);
      g_clear_error(&err);
    }
    /* Still mark successfully trashed items as deleted (partial success). */
    da_mark_deleted_paths(app, (const char **)path_arr->pdata, path_arr->len,
                          (const uint32_t *)kind_arr->data);
  }

  g_ptr_array_free(path_arr, TRUE);
  g_array_free(kind_arr, TRUE);
}

void scan_controller_delete_permanent(AppState *app) {
  GArray *nids = da_get_selected_nids_for_fileop(app);
  if (nids == NULL) {
    return;
  }
  scan_results_view_t v = scan_get_results(app->scan);
  GPtrArray *path_arr = g_ptr_array_new();
  GArray    *kind_arr = g_array_new(FALSE, FALSE, sizeof(uint32_t));
  for (guint i = 0; i < nids->len; i++) {
    size_t nid = g_array_index(nids, size_t, i);
    if (nid >= v.count || v.nodes[nid].path == NULL || v.nodes[nid].path[0] == '\0') {
      continue;
    }
    g_ptr_array_add(path_arr, (gpointer)v.nodes[nid].path);
    uint32_t k = v.nodes[nid].attributes & DISKATLAS_NODE_KIND_MASK;
    g_array_append_val(kind_arr, k);
  }
  g_array_free(nids, TRUE);

  if (path_arr->len == 0) {
    g_ptr_array_free(path_arr, TRUE);
    g_array_free(kind_arr, TRUE);
    return;
  }

  /* Confirmation dialog. */
  gchar *msg;
  if (path_arr->len == 1) {
    const gchar *name = g_path_get_basename((const gchar *)path_arr->pdata[0]);
    msg = g_strdup_printf("Permanently delete \"%s\"?\n\nThis cannot be undone.", name);
    g_free((gpointer)name);
  } else {
    msg = g_strdup_printf("Permanently delete %u items?\n\nThis cannot be undone.",
                          (unsigned)path_arr->len);
  }

  GtkWidget *dlg = gtk_message_dialog_new(GTK_WINDOW(app->window),
                                          GTK_DIALOG_MODAL | GTK_DIALOG_DESTROY_WITH_PARENT,
                                          GTK_MESSAGE_WARNING,
                                          GTK_BUTTONS_NONE,
                                          "%s", msg);
  g_free(msg);
  gtk_dialog_add_button(GTK_DIALOG(dlg), "Cancel", GTK_RESPONSE_CANCEL);
  GtkWidget *del_btn = gtk_dialog_add_button(GTK_DIALOG(dlg), "Delete Permanently",
                                              GTK_RESPONSE_ACCEPT);
  /* Style the Delete button as a destructive action. */
  GtkStyleContext *sc = gtk_widget_get_style_context(del_btn);
  gtk_style_context_add_class(sc, "destructive-action");
  gtk_dialog_set_default_response(GTK_DIALOG(dlg), GTK_RESPONSE_CANCEL);
  gtk_window_set_title(GTK_WINDOW(dlg), "Confirm Permanent Delete");

  da_message_dialog_apply_layout(dlg);

  gint resp = gtk_dialog_run(GTK_DIALOG(dlg));
  gtk_widget_destroy(dlg);

  if (resp != GTK_RESPONSE_ACCEPT) {
    g_ptr_array_free(path_arr, TRUE);
    g_array_free(kind_arr, TRUE);
    return;
  }

  GError *err = NULL;
  (void)da_file_op_delete_permanent((const char **)path_arr->pdata, path_arr->len, &err);

  if (err != NULL) {
    GtkWidget *d = gtk_message_dialog_new(GTK_WINDOW(app->window), GTK_DIALOG_MODAL,
                                          GTK_MESSAGE_ERROR, GTK_BUTTONS_OK,
                                          "Delete failed: %s", err->message);
    da_message_dialog_apply_layout(d);
    gtk_dialog_run(GTK_DIALOG(d));
    gtk_widget_destroy(d);
    g_clear_error(&err);
  }

  /* Mark all items as visually deleted regardless of success/failure
   * (the operation may have partially succeeded). */
  da_mark_deleted_paths(app, (const char **)path_arr->pdata, path_arr->len,
                        (const uint32_t *)kind_arr->data);

  g_ptr_array_free(path_arr, TRUE);
  g_array_free(kind_arr, TRUE);
}

/* ---- Rename on disk (file / tree list) ---- */

static void da_deleted_path_set_apply_rename(AppState *app, const char *old_p, const char *new_p) {
  if (app->deleted_path_set == NULL || old_p == NULL || new_p == NULL) {
    return;
  }
  size_t ol = strlen(old_p);
  GHashTable *nh = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);

  GHashTableIter hit;
  gpointer hk, hv;
  g_hash_table_iter_init(&hit, app->deleted_path_set);
  while (g_hash_table_iter_next(&hit, &hk, &hv)) {
    const char *key = (const char *)hk;
    gchar *mapped = NULL;
#if defined(G_OS_WIN32)
    if (g_ascii_strcasecmp(key, old_p) == 0) {
      mapped = g_strdup(new_p);
    } else if (strlen(key) > ol && (key[ol] == '\\' || key[ol] == '/') &&
               g_ascii_strncasecmp(key, old_p, (gssize)ol) == 0) {
      mapped = g_strdup_printf("%s%s", new_p, key + ol);
    }
#else
    if (strcmp(key, old_p) == 0) {
      mapped = g_strdup(new_p);
    } else if (strlen(key) > ol && (key[ol] == '/' || key[ol] == '\\') && strncmp(key, old_p, ol) == 0) {
      mapped = g_strdup_printf("%s%s", new_p, key + ol);
    }
#endif
    if (mapped != NULL) {
      g_hash_table_insert(nh, mapped, NULL);
    } else {
      g_hash_table_insert(nh, g_strdup(key), NULL);
    }
  }

  g_hash_table_unref(app->deleted_path_set);
  app->deleted_path_set = nh;
}

static gboolean da_rename_one_path(AppState *app, const char *old_path_utf8, const gchar *new_basename_in) {
  if (app == NULL || app->window == NULL || app->scan == NULL || old_path_utf8 == NULL || new_basename_in == NULL) {
    return FALSE;
  }
  gchar *trim = g_strdup(new_basename_in);
  if (trim == NULL) {
    return FALSE;
  }
  g_strstrip(trim);
  if (trim[0] == '\0') {
    g_free(trim);
    return FALSE;
  }
  if (strchr(trim, '/') != NULL || strchr(trim, '\\') != NULL) {
    GtkWidget *d = gtk_message_dialog_new(GTK_WINDOW(app->window), GTK_DIALOG_MODAL, GTK_MESSAGE_WARNING,
                                          GTK_BUTTONS_OK, "Name cannot contain path separators.");
    da_message_dialog_apply_layout(d);
    gtk_dialog_run(GTK_DIALOG(d));
    gtk_widget_destroy(d);
    g_free(trim);
    return FALSE;
  }

  gchar *dir = g_path_get_dirname(old_path_utf8);
  gchar *new_full = g_build_filename(dir != NULL ? dir : "", trim, NULL);
  g_free(dir);
  g_free(trim);

#if defined(G_OS_WIN32)
  gboolean same = (g_ascii_strcasecmp(old_path_utf8, new_full) == 0);
#else
  gboolean same = (strcmp(old_path_utf8, new_full) == 0);
#endif
  if (same) {
    g_free(new_full);
    return TRUE;
  }

  GFile *src = g_file_new_for_path(old_path_utf8);
  GFile *dst = g_file_new_for_path(new_full);
  GError *err = NULL;
  gboolean mv = g_file_move(src, dst, G_FILE_COPY_NONE, NULL, NULL, NULL, &err);
  g_object_unref(src);
  g_object_unref(dst);
  if (!mv) {
    GtkWidget *d = gtk_message_dialog_new(GTK_WINDOW(app->window), GTK_DIALOG_MODAL, GTK_MESSAGE_ERROR,
                                          GTK_BUTTONS_OK, "Rename failed: %s", err != NULL ? err->message : "unknown error");
    da_message_dialog_apply_layout(d);
    gtk_dialog_run(GTK_DIALOG(d));
    gtk_widget_destroy(d);
    g_clear_error(&err);
    g_free(new_full);
    return FALSE;
  }

  if (scan_result_paths_after_rename(app->scan, old_path_utf8, new_full) < 0) {
    GtkWidget *d = gtk_message_dialog_new(GTK_WINDOW(app->window), GTK_DIALOG_MODAL, GTK_MESSAGE_ERROR,
                                          GTK_BUTTONS_OK, "Renamed on disk but updating the scan snapshot failed.");
    da_message_dialog_apply_layout(d);
    gtk_dialog_run(GTK_DIALOG(d));
    gtk_widget_destroy(d);
    g_free(new_full);
    return FALSE;
  }

  da_deleted_path_set_apply_rename(app, old_path_utf8, new_full);
  da_tree_view_model_sync_entry_paths_from_scan(app);
  da_tree_view_store_refresh_path_columns(app);
  if (app->flat_list_model != NULL) {
    flat_list_model_invalidate(app->flat_list_model);
  }
  scan_controller_reclassify_mime(app);
  da_refresh_treemap(app);

  g_free(new_full);
  return TRUE;
}

void scan_controller_on_tree_name_cell_edited(AppState *app, gboolean is_tree_view, gchar *path_str, gchar *new_text) {
  if (app == NULL || !app->general_enable_rename || path_str == NULL || new_text == NULL) {
    return;
  }
  GtkTreeView *tv = is_tree_view ? GTK_TREE_VIEW(app->tree_view) : GTK_TREE_VIEW(app->tree);
  if (tv == NULL || app->scan == NULL) {
    return;
  }
  GtkTreeModel *model = gtk_tree_view_get_model(tv);
  if (model == NULL) {
    return;
  }
  GtkTreePath *tp = gtk_tree_path_new_from_string(path_str);
  if (tp == NULL) {
    return;
  }
  GtkTreeIter it;
  if (!gtk_tree_model_get_iter(model, &it, tp)) {
    gtk_tree_path_free(tp);
    return;
  }
  gtk_tree_path_free(tp);

  const gchar *old_path = NULL;
  if (is_tree_view) {
    gint64 eid = DA_TV_LP_PLACEHOLDER;
    gtk_tree_model_get(model, &it, DA_TV_COL_IDX_ID, &eid, -1);
    const char *borrowed = NULL;
    if (!da_tree_view_model_borrow_path_for_entry_id(app->tree_view_model, eid, &borrowed)) {
      return;
    }
    old_path = borrowed;
  } else {
    gint64 lp = 0;
    gtk_tree_model_get(model, &it, DA_COL_LP, &lp, -1);
    scan_results_view_t v = scan_get_results(app->scan);
    size_t nid = SIZE_MAX;
    if (!da_tree_lp_to_scan_nid(app, lp, &nid) || nid == SIZE_MAX || nid >= v.count || v.nodes == NULL) {
      return;
    }
    old_path = v.nodes[nid].path;
  }

  if (old_path == NULL || old_path[0] == '\0') {
    return;
  }

  (void)da_rename_one_path(app, old_path, new_text);
}

void scan_controller_begin_rename_selection(AppState *app) {
  if (app == NULL || !app->general_enable_rename || app->window == NULL) {
    return;
  }
  scan_results_view_t v = scan_get_results(app->scan);
  if (app->scan == NULL || v.nodes == NULL) {
    return;
  }
  scan_progress_t pr = scan_get_progress(app->scan);
  if (!pr.is_complete) {
    return;
  }

  GArray *nids = g_array_new(FALSE, FALSE, sizeof(size_t));
  if (!da_collect_explicit_selected_scan_nids(app, &v, nids) || nids->len == 0) {
    g_array_free(nids, TRUE);
    return;
  }
  size_t nid0 = g_array_index(nids, size_t, 0);
  g_array_free(nids, TRUE);
  if (nid0 >= v.count) {
    return;
  }

  GtkTreePath *tp = NULL;
  GtkTreeView *tv = NULL;
  if (scan_controller_is_file_view_tab(app)) {
    tv = GTK_TREE_VIEW(app->tree);
    if (tv == NULL || app->flat_list_model == NULL) {
      return;
    }
    GtkTreeSelection *sel = gtk_tree_view_get_selection(tv);
    GtkTreeModel *md = gtk_tree_view_get_model(tv);
    GList *rows = gtk_tree_selection_get_selected_rows(sel, &md);
    if (rows != NULL) {
      tp = gtk_tree_path_copy((GtkTreePath *)rows->data);
      g_list_free_full(rows, (GDestroyNotify)gtk_tree_path_free);
    } else if (!flat_list_model_lookup_path_for_scan_nid(app->flat_list_model, nid0, &tp) || tp == NULL) {
      return;
    }
  } else if (scan_controller_is_tree_view_tab(app)) {
    tv = GTK_TREE_VIEW(app->tree_view);
    if (tv == NULL) {
      return;
    }
    GtkTreeSelection *sel = gtk_tree_view_get_selection(tv);
    GtkTreeModel *md = gtk_tree_view_get_model(tv);
    GList *rows = gtk_tree_selection_get_selected_rows(sel, &md);
    if (rows == NULL) {
      return;
    }
    tp = gtk_tree_path_copy((GtkTreePath *)rows->data);
    g_list_free_full(rows, (GDestroyNotify)gtk_tree_path_free);
  } else {
    return;
  }

  if (tv == NULL || tp == NULL) {
    if (tp != NULL) {
      gtk_tree_path_free(tp);
    }
    return;
  }

  gtk_widget_grab_focus(GTK_WIDGET(tv));
  da_ui_name_column_begin_editing_session(tv, tp);
  gtk_tree_path_free(tp);
}

