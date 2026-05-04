#include <stdlib.h>

#include <gtk/gtk.h>

#include "treemap_widget.h"
#include "file_tree_model.h"
#include "scan_controller.h"
#include "ui_window.h"

static void append_text_column(GtkTreeView *tv, const char *title, int model_col) {
  GtkCellRenderer *r = gtk_cell_renderer_text_new();
  g_object_set(r, "ellipsize", PANGO_ELLIPSIZE_END, NULL);
  GtkTreeViewColumn *c = gtk_tree_view_column_new_with_attributes(title, r, "text", model_col, NULL);
  gtk_tree_view_column_set_resizable(c, TRUE);
  gtk_tree_view_append_column(tv, c);
}

void da_ui_build(AppState *app) {
  app->window = gtk_application_window_new(app->gtk_app);
  gtk_window_set_title(GTK_WINDOW(app->window), "DiskAtlas");
  gtk_window_set_default_size(GTK_WINDOW(app->window), 960, 620);

  GtkWidget *vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
  gtk_container_set_border_width(GTK_CONTAINER(vbox), 8);
  gtk_container_add(GTK_CONTAINER(app->window), vbox);

  /* Top: folder + scan */
  GtkWidget *top = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
  gtk_box_pack_start(GTK_BOX(vbox), top, FALSE, FALSE, 0);

  app->file_chooser_btn = gtk_file_chooser_button_new("Scan folder…", GTK_FILE_CHOOSER_ACTION_SELECT_FOLDER);
  gtk_widget_set_hexpand(app->file_chooser_btn, TRUE);
  gtk_box_pack_start(GTK_BOX(top), app->file_chooser_btn, TRUE, TRUE, 0);

  app->scan_btn = gtk_button_new_with_label("Scan");
  gtk_box_pack_start(GTK_BOX(top), app->scan_btn, FALSE, FALSE, 0);

  app->panel_scan_label = gtk_label_new("");
  gtk_label_set_xalign(GTK_LABEL(app->panel_scan_label), 0.0);
  gtk_box_pack_start(GTK_BOX(vbox), app->panel_scan_label, FALSE, FALSE, 0);

  app->progress = gtk_progress_bar_new();
  gtk_box_pack_start(GTK_BOX(vbox), app->progress, FALSE, FALSE, 0);

  /* Volume stats */
  GtkWidget *grid = gtk_grid_new();
  gtk_grid_set_column_spacing(GTK_GRID(grid), 12);
  gtk_grid_set_row_spacing(GTK_GRID(grid), 4);
  gtk_box_pack_start(GTK_BOX(vbox), grid, FALSE, FALSE, 0);

  const char *lbls[] = {"Selection:", "Total Space:", "Space Used:", "Space Free:"};
  for (int i = 0; i < 4; i++) {
    GtkWidget *l = gtk_label_new(lbls[i]);
    gtk_label_set_xalign(GTK_LABEL(l), 0.0);
    gtk_grid_attach(GTK_GRID(grid), l, 0, i, 1, 1);
  }
  app->stat_sel_val = gtk_label_new("—");
  app->stat_tot_val = gtk_label_new("—");
  app->stat_use_val = gtk_label_new("—");
  app->stat_free_val = gtk_label_new("—");
  for (int i = 0; i < 4; i++) {
    GtkWidget *w = i == 0   ? app->stat_sel_val
                   : i == 1 ? app->stat_tot_val
                   : i == 2 ? app->stat_use_val
                            : app->stat_free_val;
    gtk_label_set_xalign(GTK_LABEL(w), 0.0);
    gtk_grid_attach(GTK_GRID(grid), w, 1, i, 1, 1);
  }

  /* Search + options row */
  GtkWidget *mid = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
  gtk_box_pack_start(GTK_BOX(vbox), mid, FALSE, FALSE, 0);
  app->search = gtk_entry_new();
  gtk_entry_set_placeholder_text(GTK_ENTRY(app->search), "Filter by file name…");
  gtk_widget_set_hexpand(app->search, TRUE);
  gtk_box_pack_start(GTK_BOX(mid), app->search, TRUE, TRUE, 0);

  app->chk_dup_mtime = gtk_check_button_new_with_label("Dup clustering uses mtime");
  gtk_box_pack_start(GTK_BOX(mid), app->chk_dup_mtime, FALSE, FALSE, 0);

  gtk_box_pack_start(GTK_BOX(mid), gtk_label_new("Max list:"), FALSE, FALSE, 0);
  app->combo_display_max = gtk_combo_box_text_new();
  gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(app->combo_display_max), "All");
  gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(app->combo_display_max), "100");
  gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(app->combo_display_max), "1000");
  gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(app->combo_display_max), "10000");
  gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(app->combo_display_max), "100000");
  gtk_combo_box_set_active(GTK_COMBO_BOX(app->combo_display_max), 3);
  gtk_box_pack_start(GTK_BOX(mid), app->combo_display_max, FALSE, FALSE, 0);

  app->store = da_tree_store_new();
  app->tree = gtk_tree_view_new_with_model(GTK_TREE_MODEL(app->store));
  g_object_unref(app->store);

  const char *titles[] = {"File Name", "Path", "% of Drive", "Size", "Allocated", "Modified",
                          "Dup Count", "Dup Size", "Attributes"};
  for (int i = 0; i < DA_COL_COUNT; i++) {
    append_text_column(GTK_TREE_VIEW(app->tree), titles[i], i);
  }

  GtkWidget *sw = gtk_scrolled_window_new(NULL, NULL);
  gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(sw), GTK_POLICY_AUTOMATIC, GTK_POLICY_AUTOMATIC);
  gtk_container_add(GTK_CONTAINER(sw), app->tree);
  gtk_widget_set_vexpand(sw, TRUE);
  gtk_widget_set_hexpand(sw, TRUE);
  gtk_box_pack_start(GTK_BOX(vbox), sw, TRUE, TRUE, 0);

  GtkWidget *treemap_panel = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
  gtk_widget_set_size_request(treemap_panel, -1, 225);
  gtk_widget_set_vexpand(treemap_panel, FALSE);
  gtk_widget_set_hexpand(treemap_panel, TRUE);

  app->treemap_panel_title = gtk_label_new("Top level: —");
  gtk_label_set_xalign(GTK_LABEL(app->treemap_panel_title), 0.5);
  gtk_label_set_ellipsize(GTK_LABEL(app->treemap_panel_title), PANGO_ELLIPSIZE_MIDDLE);
  gtk_widget_set_hexpand(app->treemap_panel_title, TRUE);
  gtk_box_pack_start(GTK_BOX(treemap_panel), app->treemap_panel_title, FALSE, FALSE, 0);

  app->treemap = treemap_widget_new();
  gtk_widget_set_vexpand(app->treemap, TRUE);
  gtk_widget_set_hexpand(app->treemap, TRUE);
  gtk_box_pack_start(GTK_BOX(treemap_panel), app->treemap, TRUE, TRUE, 0);

  gtk_box_pack_start(GTK_BOX(vbox), treemap_panel, FALSE, FALSE, 0);

  app->status = gtk_label_new("");
  gtk_label_set_xalign(GTK_LABEL(app->status), 0.0);
  gtk_label_set_line_wrap(GTK_LABEL(app->status), TRUE);
  gtk_box_pack_start(GTK_BOX(vbox), app->status, FALSE, FALSE, 0);

  if (app->scan_root_utf8 != NULL) {
    gtk_file_chooser_set_filename(GTK_FILE_CHOOSER(app->file_chooser_btn), app->scan_root_utf8);
  }

  scan_controller_sync_display_max_combo(app);
  scan_controller_attach(app);
  scan_controller_refresh_volume_labels(app);

  gtk_widget_show_all(app->window);
}
