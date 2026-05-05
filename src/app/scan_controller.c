#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <glib.h>
#include <gtk/gtk.h>

#include "treemap_widget.h"
#include "diskatlas.h"
#include "file_tree_model.h"
#include "format_text.h"
#include "scan_controller.h"
#include "volumes.h"

static void app_status_set_text(AppState *app, const char *text) {
  if (app == NULL || app->status == NULL) {
    return;
  }
  const char *t = text != NULL ? text : "";
  if (GTK_IS_STATUSBAR(app->status)) {
    GtkStatusbar *sb = GTK_STATUSBAR(app->status);
    guint ctx = gtk_statusbar_get_context_id(sb, "diskatlas");
    if (app->statusbar_msg_id != 0u) {
      gtk_statusbar_remove(sb, ctx, app->statusbar_msg_id);
      app->statusbar_msg_id = 0;
    }
    if (t[0] != '\0') {
      app->statusbar_msg_id = gtk_statusbar_push(sb, ctx, t);
    }
  } else {
    gtk_label_set_text(GTK_LABEL(app->status), t);
  }
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
static gboolean on_timer_tree_chunk(gpointer data);

static void da_treemap_panel_sync_title(AppState *app) {
  if (app == NULL || app->treemap_panel_title == NULL) {
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

static void da_refresh_treemap(AppState *app) {
  da_treemap_panel_sync_title(app);
  if (app != NULL && app->treemap != NULL && TREEMAP_IS_WIDGET(app->treemap)) {
    scan_results_view_t v = {0};
    if (app->scan != NULL) {
      v = scan_get_results(app->scan);
    }
    treemap_widget_set_data(TREEMAP_WIDGET(app->treemap), app->scan_root_utf8 != NULL ? app->scan_root_utf8 : "",
                            v.nodes, v.count);
  }
}

static void on_treemap_selected(GtkWidget *treemap, gint64 scan_index, gpointer user_data) {
  AppState *app = (AppState *)user_data;
  (void)treemap;
  if (app == NULL || app->stat_sel_val == NULL) {
    return;
  }
  if (scan_index == -2) {
    gtk_label_set_text(GTK_LABEL(app->stat_sel_val), "Other (merged entries beyond treemap cap)");
    return;
  }
  if (scan_index < 0 || app->scan == NULL) {
    gtk_label_set_text(GTK_LABEL(app->stat_sel_val), "—");
    return;
  }
  {
    scan_results_view_t v = scan_get_results(app->scan);
    size_t ix = (size_t)scan_index;
    char line[1024];
    char sz[80];
    if (v.nodes == NULL || ix >= v.count) {
      gtk_label_set_text(GTK_LABEL(app->stat_sel_val), "—");
      return;
    }
    da_format_bytes(v.nodes[ix].size_bytes, sz, sizeof sz);
    snprintf(line, sizeof line, "%s  (%s)", v.nodes[ix].path != NULL ? v.nodes[ix].path : "", sz);
    gtk_label_set_text(GTK_LABEL(app->stat_sel_val), line);
  }
}

static void on_treemap_hover(GtkWidget *treemap, gint64 scan_index, gpointer user_data) {
  AppState *app = (AppState *)user_data;
  (void)treemap;
  if (app == NULL || app->status == NULL) {
    return;
  }
  if (scan_index == -1) {
    scan_controller_restore_scan_status(app);
    return;
  }
  if (scan_index == -2) {
    app_status_set_text(app, "Treemap: Other (merged entries beyond treemap tile cap)");
    return;
  }
  if (scan_index < 0 || app->scan == NULL) {
    scan_controller_restore_scan_status(app);
    return;
  }
  {
    scan_results_view_t v = scan_get_results(app->scan);
    size_t ix = (size_t)scan_index;
    char line[2048];
    char sz[80];
    if (v.nodes == NULL || ix >= v.count) {
      scan_controller_restore_scan_status(app);
      return;
    }
    da_format_bytes(v.nodes[ix].size_bytes, sz, sizeof sz);
    snprintf(line, sizeof line, "Treemap hover: %s  (%s)",
             v.nodes[ix].path != NULL ? v.nodes[ix].path : "", sz);
    app_status_set_text(app, line);
  }
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
  kill_timer(&app->timer_tree);
}

static void panel_scan_set_text(AppState *app, const char *text) {
  if (app != NULL && app->panel_scan_label != NULL && text != NULL) {
    gtk_label_set_text(GTK_LABEL(app->panel_scan_label), text);
  }
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

static void set_scan_done_status(AppState *app) {
  if (app->status == NULL || app->scan == NULL) {
    return;
  }
  scan_progress_t pr = scan_get_progress(app->scan);
  scan_results_view_t v = scan_get_results(app->scan);
  unsigned long long ntot = (unsigned long long)v.count;
  size_t pool = da_source_pool_count(app);
  size_t visible = da_source_count(app);
  unsigned long long nshow_tree = (unsigned long long)visible;
  unsigned long long npool_ll = (unsigned long long)pool;
  gboolean capped = pool > visible;
  uint32_t dgc = diskatlas_dup_max_group_id(app->scan);
  gboolean view_pool = da_view_uses_filtered_pool(app);

  char line[2048];
  if (view_pool && app->filter_build_running) {
    snprintf(line, sizeof(line),
             "Filtering… showing %llu (scan %llu)%s — dup groups %u.",
             (unsigned long long)app->filtered_count,
             (unsigned long long)(app->filter_scan_pos <= app->master_count ? app->filter_scan_pos
                                                                            : app->master_count),
             pr.is_cancel_observed ? ", cancelled scan" : "", (unsigned int)dgc);
  } else if (app->filter_active) {
    if (capped) {
      snprintf(line, sizeof(line),
               "Showing %llu of %llu matches at root (sorted by size; list cap)%s — %" PRIu64
               " bytes — dup groups %u.",
               nshow_tree, npool_ll, pr.is_cancel_observed ? ", cancelled" : "",
               (uint64_t)pr.bytes_accounted, (unsigned int)dgc);
    } else {
      snprintf(line, sizeof(line),
               "Showing %llu of %llu (by size)%s — %" PRIu64 " bytes — dup groups %u.", nshow_tree,
               ntot, pr.is_cancel_observed ? ", cancelled" : "", (uint64_t)pr.bytes_accounted,
               (unsigned int)dgc);
    }
  } else if (da_duplicates_only(app)) {
    if (capped) {
      snprintf(line, sizeof(line),
               "Showing %llu of %llu duplicate files at root (list cap)%s — %" PRIu64
               " bytes — dup groups %u.",
               nshow_tree, npool_ll, pr.is_cancel_observed ? ", cancelled" : "",
               (uint64_t)pr.bytes_accounted, (unsigned int)dgc);
    } else {
      snprintf(line, sizeof(line),
               "Showing %llu duplicate files (by size)%s — %" PRIu64 " bytes — dup groups %u.",
               nshow_tree, pr.is_cancel_observed ? ", cancelled" : "", (uint64_t)pr.bytes_accounted,
               (unsigned int)dgc);
    }
  } else {
    if (capped) {
      snprintf(line, sizeof(line),
               "Done — showing %llu of %llu entries at root (list cap)%s — %" PRIu64
               " bytes accounted — dup groups %u.",
               nshow_tree, npool_ll, pr.is_cancel_observed ? ", cancelled" : "",
               (uint64_t)pr.bytes_accounted, (unsigned int)dgc);
    } else {
      snprintf(line, sizeof(line),
               "Done — %llu entries (by size)%s — %" PRIu64 " bytes accounted — dup groups %u.",
               ntot, pr.is_cancel_observed ? ", cancelled" : "", (uint64_t)pr.bytes_accounted,
               (unsigned int)dgc);
    }
  }
  app_status_set_text(app, line);
}

void scan_controller_restore_scan_status(AppState *app) {
  set_scan_done_status(app);
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
    gboolean more = da_tree_begin_root_insert(app);
    if (more) {
      app->timer_tree = g_timeout_add(DA_TREEINSERT_MS, on_timer_tree_chunk, app);
    }
    set_scan_done_status(app);
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
    gboolean more = da_tree_begin_root_insert(app);
    if (more) {
      app->timer_tree = g_timeout_add(DA_TREEINSERT_MS, on_timer_tree_chunk, app);
    }
    set_scan_done_status(app);
    return;
  }

  app->filtered_count = 0;
  app->filter_scan_pos = 0;
  app->filter_build_running = TRUE;
  da_tree_clear(app);
  set_scan_done_status(app);
  app->timer_filter = g_timeout_add(12, on_timer_filter_chunk, app);
}

static gboolean on_timer_filter_chunk(gpointer data) {
  AppState *app = (AppState *)data;
  if (!da_view_uses_filtered_pool(app) || !app->filter_build_running || app->scan == NULL ||
      app->master_indices == NULL || app->master_count == 0 || app->filtered_indices == NULL) {
    kill_timer(&app->timer_filter);
    app->filter_build_running = FALSE;
    gboolean more = da_tree_begin_root_insert(app);
    if (more) {
      app->timer_tree = g_timeout_add(DA_TREEINSERT_MS, on_timer_tree_chunk, app);
    }
    set_scan_done_status(app);
    return G_SOURCE_REMOVE;
  }

  scan_results_view_t v = scan_get_results(app->scan);
  if (v.nodes == NULL || v.count != app->master_count) {
    kill_timer(&app->timer_filter);
    app->filter_build_running = FALSE;
    set_scan_done_status(app);
    gboolean more = da_tree_begin_root_insert(app);
    if (more) {
      app->timer_tree = g_timeout_add(DA_TREEINSERT_MS, on_timer_tree_chunk, app);
    }
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
        gboolean more = da_tree_begin_root_insert(app);
        if (more) {
          app->timer_tree = g_timeout_add(DA_TREEINSERT_MS, on_timer_tree_chunk, app);
        }
        set_scan_done_status(app);
        return G_SOURCE_REMOVE;
      }
      app->filtered_indices = nb;
      app->filtered_cap = grow;
    }
    app->filtered_indices[nf] = nid;
    app->filtered_count = nf + 1;
  }

  set_scan_done_status(app);

  if (app->filter_scan_pos >= app->master_count) {
    kill_timer(&app->timer_filter);
    app->filter_build_running = FALSE;
    set_scan_done_status(app);
    gboolean more = da_tree_begin_root_insert(app);
    if (more) {
      app->timer_tree = g_timeout_add(DA_TREEINSERT_MS, on_timer_tree_chunk, app);
    }
    return G_SOURCE_REMOVE;
  }
  return G_SOURCE_CONTINUE;
}

static void begin_populate_list(AppState *app) {
  scan_results_view_t v = scan_get_results(app->scan);

  kill_timer(&app->timer_fill);
  kill_timer(&app->timer_filter);
  kill_timer(&app->timer_tree);
  da_tree_clear(app);

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
    app->list_populated = TRUE;
    enable_scan_button(app, TRUE);
    da_refresh_treemap(app);
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
      return G_SOURCE_REMOVE;
    }
    panel_scan_set_text(app, "Sorting entries by size…");
    if (rebuild_master_index_list(app) != 0) {
      kill_timer(&app->timer_fill);
      panel_scan_set_text(app, "Could not allocate sort index.");
      app->list_populated = TRUE;
      enable_scan_button(app, TRUE);
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
    return G_SOURCE_REMOVE;
  }

  kill_timer(&app->timer_fill);

  app->list_populated = TRUE;
  char finish_pan[256];
  scan_progress_t pr_done = scan_get_progress(app->scan);
  snprintf(finish_pan, sizeof(finish_pan), "Scan complete in %.2f seconds%s", app->last_scan_elapsed_s,
           pr_done.is_cancel_observed ? " (cancelled)" : "");
  panel_scan_set_text(app, finish_pan);
  set_scan_done_status(app);
  enable_scan_button(app, TRUE);

  gboolean more = da_tree_begin_root_insert(app);
  if (more) {
    app->timer_tree = g_timeout_add(DA_TREEINSERT_MS, on_timer_tree_chunk, app);
  } else {
    da_refresh_treemap(app);
  }

  const gchar *peek = gtk_entry_get_text(GTK_ENTRY(app->search));
  gboolean search_nonempty = (peek != NULL && peek[0] != '\0');
  if (search_nonempty || da_duplicates_only(app)) {
    apply_search_filter(app);
  }
  return G_SOURCE_REMOVE;
}

static gboolean on_timer_scan_tick(gpointer data) {
  AppState *app = (AppState *)data;
  if (app->scan == NULL) {
    kill_timer(&app->timer_scan);
    return G_SOURCE_REMOVE;
  }
  scan_progress_t pr = scan_get_progress(app->scan);
  char folders_buf[32];
  char files_buf[32];
  char buf[512];
  da_format_uint64_locale(pr.folder_count, folders_buf, sizeof(folders_buf));
  da_format_uint64_locale(pr.file_count, files_buf, sizeof(files_buf));
  snprintf(buf, sizeof(buf), "Scanning… (Folders: %s  Files: %s)", folders_buf, files_buf);
  panel_scan_set_text(app, buf);
  scan_progress_set_indeterminate(app, TRUE);

  if (pr.is_complete) {
    kill_timer(&app->timer_scan);
    gint64 now = g_get_monotonic_time();
    app->last_scan_elapsed_s = (double)(now - app->scan_start_us) / 1000000.0;
    scan_progress_set_full(app);
    gtk_button_set_label(GTK_BUTTON(app->scan_btn), "Scan");
    enable_scan_button(app, FALSE);
    begin_populate_list(app);
    return G_SOURCE_REMOVE;
  }
  return G_SOURCE_CONTINUE;
}

static gboolean on_timer_tree_chunk(gpointer data) {
  AppState *app = (AppState *)data;
  if (da_tree_insert_roots_chunk(app)) {
    return G_SOURCE_CONTINUE;
  }
  kill_timer(&app->timer_tree);
  da_refresh_treemap(app);
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
  if (app == NULL || app->scan_root_utf8 == NULL) {
    return;
  }

  kill_all_timers(app);
  da_tree_clear(app);
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
    scan_result_free(app->scan);
    app->scan = NULL;
  }

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
#define DISKATLAS_APP_ENABLE_NTFS_MFT 0
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
    return;
  }

  app->scan_start_us = g_get_monotonic_time();
  scan_button_set_cancelling_mode(app);
  scan_progress_set_indeterminate(app, TRUE);
  app->timer_scan = g_timeout_add(120, on_timer_scan_tick, app);
  panel_scan_set_text(app, "Starting scan…");
}

static void on_scan_clicked(GtkButton *btn, gpointer user_data) {
  (void)btn;
  AppState *app = (AppState *)user_data;
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

static void on_file_set(GtkFileChooserButton *b, gpointer user_data) {
  (void)b;
  AppState *app = (AppState *)user_data;
  gchar *fn = gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(app->file_chooser_btn));
  if (fn == NULL) {
    return;
  }
  free(app->scan_root_utf8);
  app->scan_root_utf8 = g_strdup(fn);
  g_free(fn);
  scan_controller_refresh_volume_labels(app);
  da_refresh_treemap(app);
}

static void on_show_folders_toggled(GtkToggleButton *btn, gpointer user_data) {
  (void)btn;
  AppState *app = (AppState *)user_data;
  if (app == NULL || !app->list_populated || app->scan == NULL) {
    return;
  }

  kill_timer(&app->timer_fill);
  kill_timer(&app->timer_filter);
  kill_timer(&app->timer_tree);

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
  da_refresh_treemap(app);
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

void scan_controller_refresh_volume_labels(AppState *app) {
  da_treemap_panel_sync_title(app);
  if (app->scan_root_utf8 == NULL || app->scan_root_utf8[0] == '\0') {
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
    return;
  }
  app->volume_total_bytes = tot;
  app->volume_pct_denominator_bytes = (used_b > 0u) ? used_b : tot;
  char a[64];
  char use_line[160];
  char free_line[160];
  da_format_bytes(tot, a, sizeof(a));
  da_format_bytes_with_pct(used_b, tot, use_line, sizeof(use_line));
  da_format_bytes_with_pct(free_b, tot, free_line, sizeof(free_line));
  char sel_line[640];
  da_volume_selection_label(app->scan_root_utf8, sel_line, sizeof(sel_line));
  if (app->stat_sel_val) {
    gtk_label_set_text(GTK_LABEL(app->stat_sel_val), sel_line[0] != '\0' ? sel_line : app->scan_root_utf8);
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
}

static void on_tree_row_expanded(GtkTreeView *tv, GtkTreeIter *iter, GtkTreePath *path,
                                 gpointer user_data) {
  da_tree_on_row_expanded(tv, iter, path, user_data);
}

void scan_controller_attach(AppState *app) {
  g_signal_connect(app->scan_btn, "clicked", G_CALLBACK(on_scan_clicked), app);
  g_signal_connect(app->file_chooser_btn, "file-set", G_CALLBACK(on_file_set), app);
  g_signal_connect(app->combo_display_max, "changed", G_CALLBACK(on_combo_display_changed), app);
  if (app->duplicates_only_check != NULL) {
    g_signal_connect(app->duplicates_only_check, "toggled", G_CALLBACK(on_duplicates_only_toggled), app);
  }
  if (app->show_folders_check != NULL) {
    g_signal_connect(app->show_folders_check, "toggled", G_CALLBACK(on_show_folders_toggled), app);
  }
  g_signal_connect(app->search, "changed", G_CALLBACK(on_search_changed), app);
  g_signal_connect(app->tree, "row-expanded", G_CALLBACK(on_tree_row_expanded), app);
  if (app->treemap != NULL && TREEMAP_IS_WIDGET(app->treemap)) {
    treemap_widget_set_selected_callback(TREEMAP_WIDGET(app->treemap), on_treemap_selected, app);
    treemap_widget_set_hover_callback(TREEMAP_WIDGET(app->treemap), on_treemap_hover, app);
  }
}

void scan_controller_detach(AppState *app) {
  kill_all_timers(app);
}
