#include <stdlib.h>
#include <stdint.h>
#include <string.h>

#include <glib.h>
#include <gtk/gtk.h>

#include "diskatlas.h"

#include "treemap_widget.h"
#include "file_tree_model.h"
#include "scan_controller.h"
#include "ui_window.h"
#include "volumes.h"
#include "da_cell_renderer_progress.h"
#include "file_tree_sort.h"
#include "shell_icon.h"

#define DISKATLAS_WINDOW_UI_RESOURCE "/ui/diskatlas_window.ui"
/** gtk_widget_set_name for CSS; targets percent column progress rendering only on this tree. */
#define DISKATLAS_FILE_TREE_CSS_NAME "diskatlas_file_view_tree"

static void da_install_file_tree_progress_css(GtkWidget *tree) {
  GtkCssProvider *provider = gtk_css_provider_new();
  /* GtkCellRendererProgress: trough = full cell; progressbar = filled segment only (gtkcellrendererprogress.c). */
  const char *css =
      "#" DISKATLAS_FILE_TREE_CSS_NAME ".view.trough {\n"
      "  background-color: transparent;\n"
      "  border: 1px solid alpha(@theme_fg_color, 0.35);\n"
      "}\n"
      "#" DISKATLAS_FILE_TREE_CSS_NAME ".view.progressbar {\n"
      "  background-color: #3584e4;\n"
      "  background-image: none;\n"
      "  border: none;\n"
      "  box-shadow: none;\n"
      "  color: @theme_fg_color;\n"
      "}\n"
      "#" DISKATLAS_FILE_TREE_CSS_NAME ".view.progressbar:selected {\n"
      "  color: @theme_selected_fg_color;\n"
      "}\n";
  GError *css_err = NULL;
  if (!gtk_css_provider_load_from_data(provider, css, -1, &css_err)) {
    g_warning("DiskAtlas file tree CSS: %s", css_err != NULL ? css_err->message : "unknown");
    g_clear_error(&css_err);
    g_object_unref(provider);
    return;
  }
  gtk_style_context_add_provider(gtk_widget_get_style_context(tree), GTK_STYLE_PROVIDER(provider),
                                 GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
  g_object_unref(provider);
}

/** Matches file_tree_model.c placeholder row LP. */
#define DA_TREE_LP_PLACEHOLDER ((gint64)INT64_MIN)

static gboolean da_lp_to_nid(gint64 lp, AppState *app, size_t *out_nid) {
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

static void file_name_icon_cell_data(GtkTreeViewColumn *column, GtkCellRenderer *cell, GtkTreeModel *model,
                                     GtkTreeIter *iter, gpointer user_data) {
  (void)column;
  AppState *app = user_data;
  const char *icon_name = "text-x-generic";
  gint64 lp = 0;
  gchar *path_utf8 = NULL;
  gtk_tree_model_get(model, iter, DA_COL_LP, &lp, 1, &path_utf8, -1);

  if (app != NULL && app->scan != NULL) {
    if (lp == DA_TREE_LP_PLACEHOLDER) {
      icon_name = NULL;
    } else {
      size_t nid = 0;
      if (da_lp_to_nid(lp, app, &nid) && nid != SIZE_MAX) {
        scan_results_view_t v = scan_get_results(app->scan);
        if (v.nodes != NULL && nid < v.count) {
          uint32_t kind = v.nodes[nid].attributes & DISKATLAS_NODE_KIND_MASK;
          if (kind == DISKATLAS_NODE_KIND_DIR) {
            icon_name = "folder";
          } else if (kind == DISKATLAS_NODE_KIND_SYMLINK) {
            icon_name = "inode-symlink";
          } else {
            icon_name = "text-x-generic";
          }
        }
      }
    }
  }

  GdkPixbuf *pb = NULL;
  if (path_utf8 != NULL && path_utf8[0] != '\0') {
    pb = da_shell_icon_for_path(path_utf8, 16);
  }

  if (pb == NULL && icon_name != NULL) {
    GtkIconTheme *theme = gtk_icon_theme_get_default();
    pb = gtk_icon_theme_load_icon(theme, icon_name, 16, GTK_ICON_LOOKUP_FORCE_SIZE, NULL);
    if (pb == NULL && strcmp(icon_name, "inode-symlink") == 0) {
      pb = gtk_icon_theme_load_icon(theme, "text-x-generic", 16, GTK_ICON_LOOKUP_FORCE_SIZE, NULL);
    }
  }

  g_free(path_utf8);

  g_object_set(GTK_CELL_RENDERER_PIXBUF(cell), "pixbuf", pb, NULL);
  if (pb != NULL) {
    g_object_unref(pb);
  }
}

static void append_file_name_column(GtkTreeView *tv, AppState *app, const char *title, int sort_model_id, int width_px,
                                    int min_width_px) {
  GtkCellRenderer *pix = gtk_cell_renderer_pixbuf_new();
  GtkCellRenderer *txt = gtk_cell_renderer_text_new();
  g_object_set(txt, "ellipsize", PANGO_ELLIPSIZE_END, "xalign", 0.0f, NULL);

  GtkTreeViewColumn *c = gtk_tree_view_column_new();
  gtk_tree_view_column_set_title(c, title);
  gtk_tree_view_column_pack_start(c, pix, FALSE);
  gtk_tree_view_column_set_cell_data_func(c, pix, file_name_icon_cell_data, app, NULL);
  gtk_tree_view_column_pack_start(c, txt, TRUE);
  gtk_tree_view_column_add_attribute(c, txt, "text", 0);

  gtk_tree_view_column_set_alignment(c, 0.0f);
  gtk_tree_view_column_set_resizable(c, TRUE);
  gtk_tree_view_column_set_sizing(c, GTK_TREE_VIEW_COLUMN_FIXED);
  gtk_tree_view_column_set_min_width(c, min_width_px);
  gtk_tree_view_column_set_fixed_width(c, width_px);
  gtk_tree_view_column_set_sort_column_id(c, sort_model_id);
  gtk_tree_view_append_column(tv, c);
}

static void on_search_clear_clicked(GtkButton *btn, gpointer user_data) {
  (void)btn;
  AppState *app = (AppState *)user_data;
  if (app->search != NULL) {
    gtk_entry_set_text(GTK_ENTRY(app->search), "");
  }
}

static void pct_of_drive_cell_data(GtkTreeViewColumn *column, GtkCellRenderer *cell, GtkTreeModel *model,
                                   GtkTreeIter *iter, gpointer user_data) {
  (void)column;
  (void)user_data;
  gint pv = -1;
  gchar *txt = NULL;
  gtk_tree_model_get(model, iter, DA_COL_PCT, &pv, 2, &txt, -1);
  gint v = 0;
  if (pv >= 0) {
    v = pv > 100 ? 100 : pv;
  }
  g_object_set(GTK_CELL_RENDERER_PROGRESS(cell), "value", v, "text", txt != NULL ? txt : "", "text-xalign",
               0.92f, NULL);
  g_free(txt);
}

static void append_pct_of_drive_column(GtkTreeView *tv, const char *title, int sort_model_id, int width_px,
                                       int min_width_px) {
  GtkCellRenderer *r = da_cell_renderer_progress_new();
  g_object_set(r, "xpad", 10, NULL);
  GtkTreeViewColumn *c = gtk_tree_view_column_new();
  gtk_tree_view_column_set_title(c, title);
  gtk_tree_view_column_pack_start(c, r, TRUE);
  gtk_tree_view_column_set_cell_data_func(c, r, pct_of_drive_cell_data, NULL, NULL);
  gtk_tree_view_column_set_alignment(c, 1.0f);
  gtk_tree_view_column_set_resizable(c, TRUE);
  gtk_tree_view_column_set_sizing(c, GTK_TREE_VIEW_COLUMN_FIXED);
  gtk_tree_view_column_set_min_width(c, min_width_px);
  gtk_tree_view_column_set_fixed_width(c, width_px);
  gtk_tree_view_column_set_sort_column_id(c, sort_model_id);
  gtk_tree_view_append_column(tv, c);
}

static void append_text_column(GtkTreeView *tv, const char *title, int model_col, int sort_model_id, int width_px,
                               int min_width_px, gfloat xalign) {
  GtkCellRenderer *r = gtk_cell_renderer_text_new();
  g_object_set(r, "ellipsize", PANGO_ELLIPSIZE_END, "xalign", xalign, NULL);
  GtkTreeViewColumn *c = gtk_tree_view_column_new_with_attributes(title, r, "text", model_col, NULL);
  gtk_tree_view_column_set_alignment(c, xalign);
  gtk_tree_view_column_set_resizable(c, TRUE);
  gtk_tree_view_column_set_sizing(c, GTK_TREE_VIEW_COLUMN_FIXED);
  gtk_tree_view_column_set_min_width(c, min_width_px);
  gtk_tree_view_column_set_fixed_width(c, width_px);
  gtk_tree_view_column_set_sort_column_id(c, sort_model_id);
  gtk_tree_view_append_column(tv, c);
}

void da_ui_build(AppState *app) {
  GtkBuilder *builder = gtk_builder_new();
  GError *err = NULL;
  if (!gtk_builder_add_from_resource(builder, DISKATLAS_WINDOW_UI_RESOURCE, &err)) {
    g_error("Failed to load UI (%s): %s", DISKATLAS_WINDOW_UI_RESOURCE, err->message);
  }

  app->window = GTK_WIDGET(gtk_builder_get_object(builder, "main_window"));
  gtk_window_set_application(GTK_WINDOW(app->window), app->gtk_app);

  app->file_chooser_btn = GTK_WIDGET(gtk_builder_get_object(builder, "file_chooser_btn"));

#if defined(_WIN32)
  {
    GtkWidget *old_btn = app->file_chooser_btn;
    gchar *prev = gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(old_btn));
    GtkWidget *grid = gtk_widget_get_parent(old_btn);
    gtk_container_remove(GTK_CONTAINER(grid), old_btn);

    GtkWidget *dlg =
        gtk_file_chooser_dialog_new("Scan folder…", GTK_WINDOW(app->window), GTK_FILE_CHOOSER_ACTION_SELECT_FOLDER,
                                    "_Cancel", GTK_RESPONSE_CANCEL, "_Select", GTK_RESPONSE_ACCEPT, NULL);
    da_win32_file_chooser_set_drive_places_only(GTK_FILE_CHOOSER(dlg));

    GtkWidget *new_btn = gtk_file_chooser_button_new_with_dialog(dlg);
    gtk_widget_set_hexpand(new_btn, TRUE);
    gtk_widget_set_can_focus(new_btn, FALSE);
    gtk_file_chooser_button_set_title(GTK_FILE_CHOOSER_BUTTON(new_btn), "Scan folder…");

    gtk_grid_attach(GTK_GRID(grid), new_btn, 1, 0, 1, 1);
    gtk_widget_show(new_btn);

    if (prev != NULL) {
      gtk_file_chooser_set_filename(GTK_FILE_CHOOSER(new_btn), prev);
      g_free(prev);
    }

    gtk_widget_destroy(old_btn);
    app->file_chooser_btn = new_btn;
  }
#endif

  app->scan_btn = GTK_WIDGET(gtk_builder_get_object(builder, "scan_btn"));
  app->panel_scan_label = GTK_WIDGET(gtk_builder_get_object(builder, "panel_scan_label"));
  app->progress = GTK_WIDGET(gtk_builder_get_object(builder, "progress"));
  app->search = GTK_WIDGET(gtk_builder_get_object(builder, "search"));
  app->chk_dup_mtime = GTK_WIDGET(gtk_builder_get_object(builder, "chk_dup_mtime"));
  app->combo_display_max = GTK_WIDGET(gtk_builder_get_object(builder, "combo_display_max"));
  app->tree = GTK_WIDGET(gtk_builder_get_object(builder, "file_view_tree"));
  gtk_widget_set_name(app->tree, DISKATLAS_FILE_TREE_CSS_NAME);
  app->treemap_panel_title = GTK_WIDGET(gtk_builder_get_object(builder, "treemap_panel_title"));
  app->status = GTK_WIDGET(gtk_builder_get_object(builder, "main_statusbar"));

  app->stat_sel_val = GTK_WIDGET(gtk_builder_get_object(builder, "stat_sel_val"));
  app->stat_tot_val = GTK_WIDGET(gtk_builder_get_object(builder, "stat_tot_val"));
  app->stat_use_val = GTK_WIDGET(gtk_builder_get_object(builder, "stat_use_val"));
  app->stat_free_val = GTK_WIDGET(gtk_builder_get_object(builder, "stat_free_val"));

  GtkWidget *tree_scrolled = GTK_WIDGET(gtk_builder_get_object(builder, "file_view_scrolled"));
  gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(tree_scrolled), GTK_POLICY_AUTOMATIC, GTK_POLICY_AUTOMATIC);

  GtkWidget *search_clear_btn = GTK_WIDGET(gtk_builder_get_object(builder, "search_clear_btn"));
  if (search_clear_btn != NULL) {
    g_signal_connect(search_clear_btn, "clicked", G_CALLBACK(on_search_clear_clicked), app);
  }

  GtkWidget *treemap_slot = GTK_WIDGET(gtk_builder_get_object(builder, "treemap_slot"));
  app->treemap = treemap_widget_new();
  gtk_box_pack_start(GTK_BOX(treemap_slot), app->treemap, TRUE, TRUE, 0);

  gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(app->combo_display_max), "All");
  gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(app->combo_display_max), "100");
  gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(app->combo_display_max), "1000");
  gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(app->combo_display_max), "10000");
  gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(app->combo_display_max), "100000");
  gtk_combo_box_set_active(GTK_COMBO_BOX(app->combo_display_max), 3);

  app->store = da_tree_store_new();
  gtk_tree_view_set_model(GTK_TREE_VIEW(app->tree), GTK_TREE_MODEL(app->store));
  g_object_unref(app->store);

  const char *titles[] = {"File Name", "Path", "Percent of Drive", "Size", "Allocated", "Modified",
                          "Dup Count", "Dup Size", "Attributes"};
  const int col_w[] = {248, 480, 110, 100, 100, 140, 80, 100, 68};
  const int col_min_w[] = {140, 200, 88, 72, 72, 100, 56, 72, 48};
  const int col_sort_id[] = {0, 1, DA_COL_PCT, 3, 4, 5, 6, 7, 8};
  for (int i = 0; i < DA_COL_COUNT; i++) {
    if (i == 0) {
      append_file_name_column(GTK_TREE_VIEW(app->tree), app, titles[i], col_sort_id[i], col_w[i], col_min_w[i]);
      continue;
    }
    if (i == 2) {
      append_pct_of_drive_column(GTK_TREE_VIEW(app->tree), titles[i], col_sort_id[i], col_w[i], col_min_w[i]);
      continue;
    }
    gfloat xalign = 0.0f;
    if (i == 3 || i == 4 || i == 6 || i == 7) {
      xalign = 1.0f;
    }
    append_text_column(GTK_TREE_VIEW(app->tree), titles[i], i, col_sort_id[i], col_w[i], col_min_w[i], xalign);
  }

  da_file_tree_install_sorting(GTK_TREE_VIEW(app->tree), app->store, app);

  da_install_file_tree_progress_css(app->tree);

  g_object_unref(builder);

  if (app->scan_root_utf8 != NULL) {
    gtk_file_chooser_set_filename(GTK_FILE_CHOOSER(app->file_chooser_btn), app->scan_root_utf8);
  }

  scan_controller_sync_display_max_combo(app);
  scan_controller_attach(app);
  scan_controller_refresh_volume_labels(app);

  gtk_widget_show_all(app->window);
}
