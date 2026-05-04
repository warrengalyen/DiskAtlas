#include <stdlib.h>

#include <glib.h>
#include <gtk/gtk.h>

#include "treemap_widget.h"
#include "file_tree_model.h"
#include "scan_controller.h"
#include "ui_window.h"

#define DISKATLAS_WINDOW_UI_RESOURCE "/ui/diskatlas_window.ui"

static void append_text_column(GtkTreeView *tv, const char *title, int model_col, int width_px) {
  GtkCellRenderer *r = gtk_cell_renderer_text_new();
  g_object_set(r, "ellipsize", PANGO_ELLIPSIZE_END, NULL);
  GtkTreeViewColumn *c = gtk_tree_view_column_new_with_attributes(title, r, "text", model_col, NULL);
  gtk_tree_view_column_set_resizable(c, TRUE);
  gtk_tree_view_column_set_sizing(c, GTK_TREE_VIEW_COLUMN_FIXED);
  gtk_tree_view_column_set_fixed_width(c, width_px);
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
  app->scan_btn = GTK_WIDGET(gtk_builder_get_object(builder, "scan_btn"));
  app->panel_scan_label = GTK_WIDGET(gtk_builder_get_object(builder, "panel_scan_label"));
  app->progress = GTK_WIDGET(gtk_builder_get_object(builder, "progress"));
  app->search = GTK_WIDGET(gtk_builder_get_object(builder, "search"));
  app->chk_dup_mtime = GTK_WIDGET(gtk_builder_get_object(builder, "chk_dup_mtime"));
  app->combo_display_max = GTK_WIDGET(gtk_builder_get_object(builder, "combo_display_max"));
  app->tree = GTK_WIDGET(gtk_builder_get_object(builder, "tree"));
  app->treemap_panel_title = GTK_WIDGET(gtk_builder_get_object(builder, "treemap_panel_title"));
  app->status = GTK_WIDGET(gtk_builder_get_object(builder, "status"));
  app->stat_sel_val = GTK_WIDGET(gtk_builder_get_object(builder, "stat_sel_val"));
  app->stat_tot_val = GTK_WIDGET(gtk_builder_get_object(builder, "stat_tot_val"));
  app->stat_use_val = GTK_WIDGET(gtk_builder_get_object(builder, "stat_use_val"));
  app->stat_free_val = GTK_WIDGET(gtk_builder_get_object(builder, "stat_free_val"));

  GtkWidget *tree_scrolled = GTK_WIDGET(gtk_builder_get_object(builder, "tree_scrolled"));
  gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(tree_scrolled), GTK_POLICY_AUTOMATIC, GTK_POLICY_AUTOMATIC);

  GtkWidget *treemap_slot = GTK_WIDGET(gtk_builder_get_object(builder, "treemap_slot"));
  app->treemap = treemap_widget_new();
  gtk_box_pack_start(GTK_BOX(treemap_slot), app->treemap, TRUE, TRUE, 0);

  gtk_label_set_line_wrap(GTK_LABEL(app->status), TRUE);

  gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(app->combo_display_max), "All");
  gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(app->combo_display_max), "100");
  gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(app->combo_display_max), "1000");
  gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(app->combo_display_max), "10000");
  gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(app->combo_display_max), "100000");
  gtk_combo_box_set_active(GTK_COMBO_BOX(app->combo_display_max), 3);

  app->store = da_tree_store_new();
  gtk_tree_view_set_model(GTK_TREE_VIEW(app->tree), GTK_TREE_MODEL(app->store));
  g_object_unref(app->store);

  const char *titles[] = {"File Name", "Path", "% of Drive", "Size", "Allocated", "Modified",
                          "Dup Count", "Dup Size", "Attributes"};
  const int col_w[] = {220, 480, 110, 100, 100, 140, 80, 100, 68};
  for (int i = 0; i < DA_COL_COUNT; i++) {
    append_text_column(GTK_TREE_VIEW(app->tree), titles[i], i, col_w[i]);
  }

  g_object_unref(builder);

  if (app->scan_root_utf8 != NULL) {
    gtk_file_chooser_set_filename(GTK_FILE_CHOOSER(app->file_chooser_btn), app->scan_root_utf8);
  }

  scan_controller_sync_display_max_combo(app);
  scan_controller_attach(app);
  scan_controller_refresh_volume_labels(app);

  gtk_widget_show_all(app->window);
}
