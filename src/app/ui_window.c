#include <stdlib.h>
#include <stdint.h>
#include <string.h>

#include <glib.h>
#include <gdk/gdk.h>
#include <gtk/gtk.h>

#include "diskatlas.h"

#include "treemap_widget.h"
#include "file_tree_model.h"
#include "tree_view_model.h"
#include "scan_controller.h"
#include "scan_source_combo.h"
#include "ui_window.h"
#include "volumes.h"
#include "da_cell_renderer_progress.h"
#include "flat_list_model.h"
#include "shell_icon.h"

#if defined(G_OS_WIN32)

static void on_dont_show_admin_ntfs_toggled(GtkToggleButton *tb, gpointer user_data) {
  AppState *app = (AppState *)user_data;
  (void)tb;
  if (gtk_toggle_button_get_active(tb)) {
    da_win32_set_admin_ntfs_notice_hidden(TRUE);
    if (app->admin_ntfs_notice_panel != NULL) {
      gtk_widget_hide(app->admin_ntfs_notice_panel);
    }
  }
}

static void on_restart_admin_clicked(GtkButton *btn, gpointer user_data) {
  (void)btn;
  AppState *app = (AppState *)user_data;
  if (app->dont_show_again_check != NULL &&
      gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(app->dont_show_again_check))) {
    da_win32_set_admin_ntfs_notice_hidden(TRUE);
  }
  if (da_win32_restart_elevated_self() && app->gtk_app != NULL) {
    g_application_quit(G_APPLICATION(app->gtk_app));
  }
}
#endif

/** GtkPaned limits — align with diskatlas_window.ui (tree_view_paned min-content-height, treemap_panel height-request). */
#define DA_FILE_VIEW_PANED_MIN_TREE 70
#define DA_FILE_VIEW_PANED_MIN_TREEMAP 225
#define DA_FILE_VIEW_PANED_HANDLE_ROUGH 10
/** Reserve a few px so paned allocation vs. shadow/border doesn’t sit exactly on the scroll min edge. */
#define DA_FILE_VIEW_PANED_LAYOUT_SLACK 4

static void da_tree_scrolled_clamp_vadjustment(GtkWidget *tree_scrolled) {
  GtkWidget *child = gtk_bin_get_child(GTK_BIN(tree_scrolled));
  if (child == NULL || !GTK_IS_SCROLLABLE(child)) {
    return;
  }
  GtkAdjustment *v = gtk_scrollable_get_vadjustment(GTK_SCROLLABLE(child));
  if (v == NULL) {
    return;
  }
  gdouble lo = gtk_adjustment_get_lower(v);
  gdouble upper = gtk_adjustment_get_upper(v);
  gdouble page = gtk_adjustment_get_page_size(v);
  gdouble max_val = upper - page;
  if (max_val < lo) {
    max_val = lo;
  }
  gdouble val = gtk_adjustment_get_value(v);
  if (val > max_val) {
    gtk_adjustment_set_value(v, max_val);
  } else if (val < lo) {
    gtk_adjustment_set_value(v, lo);
  }
}

static void da_tree_scrolled_on_allocate(GtkWidget *widget, GdkRectangle *allocation, gpointer user_data) {
  (void)allocation;
  (void)user_data;
  da_tree_scrolled_clamp_vadjustment(widget);
}

static void da_file_view_paned_clamp_position(GtkPaned *paned) {
  GtkWidget *w = GTK_WIDGET(paned);
  gint h = gtk_widget_get_allocated_height(w);
  if (h <= 1) {
    return;
  }
  gint pos = gtk_paned_get_position(paned);
  gint min_top = DA_FILE_VIEW_PANED_MIN_TREE;
  gint min_bottom = DA_FILE_VIEW_PANED_MIN_TREEMAP;
  gint handle = DA_FILE_VIEW_PANED_HANDLE_ROUGH;
  gint max_pos = h - min_bottom - handle - DA_FILE_VIEW_PANED_LAYOUT_SLACK;
  if (max_pos < min_top) {
    max_pos = min_top;
  }
  gint clamped = pos;
  if (clamped < min_top) {
    clamped = min_top;
  }
  if (clamped > max_pos) {
    clamped = max_pos;
  }
  if (clamped != pos) {
    gtk_paned_set_position(paned, clamped);
  }
}

static void da_file_view_paned_on_notify_position(GObject *object, GParamSpec *pspec, gpointer user_data) {
  (void)pspec;
  da_file_view_paned_clamp_position(GTK_PANED(object));
  if (user_data != NULL) {
    da_tree_scrolled_clamp_vadjustment(GTK_WIDGET(user_data));
  }
}

static void da_file_view_paned_on_allocate(GtkWidget *widget, GdkRectangle *allocation, gpointer user_data) {
  (void)allocation;
  da_file_view_paned_clamp_position(GTK_PANED(widget));
  if (user_data != NULL) {
    da_tree_scrolled_clamp_vadjustment(GTK_WIDGET(user_data));
  }
}

#define DISKATLAS_WINDOW_UI_RESOURCE "/ui/diskatlas_window.ui"
#define DISKATLAS_APP_CSS_RESOURCE "/app.css"
/** gtk_style_context_add_class for percent-column progress CSS (file list + Tree View tabs). */
#define DISKATLAS_TREE_PROGRESS_STYLE_CLASS "diskatlas-tree-progress"
/** Default GtkCellRendererText background for Allocated columns (file list + Tree View tab). */
#define DISKATLAS_TREE_ALLOC_CELL_BG "#EFEFEF"

static void da_load_global_app_css(void) {
  static gboolean installed = FALSE;
  if (installed) {
    return;
  }
  installed = TRUE;

  GtkCssProvider *provider = gtk_css_provider_new();
  gtk_css_provider_load_from_resource(provider, DISKATLAS_APP_CSS_RESOURCE);
  gtk_style_context_add_provider_for_screen(gdk_screen_get_default(), GTK_STYLE_PROVIDER(provider),
                                            GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
  g_object_unref(provider);
}

/** Matches file_tree_model.c placeholder row LP. */

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
      if (da_tree_lp_to_scan_nid(app, lp, &nid) && nid != SIZE_MAX) {
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
  (void)user_data;
  gint pv = -1;
  gchar *txt = NULL;
  gtk_tree_model_get(model, iter, DA_COL_PCT, &pv, 2, &txt, -1);
  gint v = 0;
  if (pv >= 0) {
    v = pv > 100 ? 100 : pv;
  }

  gint col_w = gtk_tree_view_column_get_width(column);
  if (col_w <= 1) {
    col_w = gtk_tree_view_column_get_fixed_width(column);
  }
  if (col_w > 4) {
    gtk_cell_renderer_set_fixed_size(cell, col_w - 4, -1);
  } else {
    gtk_cell_renderer_set_fixed_size(cell, -1, -1);
  }

  g_object_set(GTK_CELL_RENDERER_PROGRESS(cell), "value", v, "text", txt != NULL ? txt : "", "text-xalign",
               0.98f, NULL);
  g_free(txt);
}

static void append_pct_of_drive_column(GtkTreeView *tv, const char *title, int sort_model_id, int width_px,
                                       int min_width_px) {
  GtkCellRenderer *r = da_cell_renderer_progress_new();
  g_object_set(r, "xpad", 0, "ypad", 0, "xalign", 0.0f, NULL);
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
                               int min_width_px, gfloat xalign, const char *cell_background) {
  GtkCellRenderer *r = gtk_cell_renderer_text_new();
  g_object_set(r, "ellipsize", PANGO_ELLIPSIZE_END, "xalign", xalign, NULL);
  if (cell_background != NULL) {
    g_object_set(r, "background", cell_background, "background-set", TRUE, NULL);
  }
  GtkTreeViewColumn *c = gtk_tree_view_column_new_with_attributes(title, r, "text", model_col, NULL);
  gtk_tree_view_column_set_alignment(c, xalign);
  gtk_tree_view_column_set_resizable(c, TRUE);
  gtk_tree_view_column_set_sizing(c, GTK_TREE_VIEW_COLUMN_FIXED);
  gtk_tree_view_column_set_min_width(c, min_width_px);
  gtk_tree_view_column_set_fixed_width(c, width_px);
  gtk_tree_view_column_set_sort_column_id(c, sort_model_id);
  gtk_tree_view_append_column(tv, c);
}

/* ---- Tree View tab helpers ---- */

static void tv_icon_cell_data(GtkTreeViewColumn *column, GtkCellRenderer *cell, GtkTreeModel *model,
                              GtkTreeIter *iter, gpointer user_data) {
  (void)column;
  (void)user_data;
  gint64 eid = 0;
  gchar *path_utf8 = NULL;
  guint kind = 0;
  gtk_tree_model_get(model, iter,
                     DA_TV_COL_IDX_ID, &eid,
                     DA_TV_COL_PATH,   &path_utf8,
                     DA_TV_COL_KIND,   &kind,
                     -1);

  const char *icon_name = NULL;
  if (eid != DA_TV_LP_PLACEHOLDER) {
    if (kind == DISKATLAS_NODE_KIND_DIR) {
      icon_name = "folder";
    } else if (kind == DISKATLAS_NODE_KIND_SYMLINK) {
      icon_name = "inode-symlink";
    } else {
      icon_name = "text-x-generic";
    }
  }

  GdkPixbuf *pb = NULL;
  if (path_utf8 != NULL && path_utf8[0] != '\0' && eid != DA_TV_LP_PLACEHOLDER) {
    pb = da_shell_icon_for_path(path_utf8, 16);
  }
  if (pb == NULL && icon_name != NULL) {
    GtkIconTheme *theme = gtk_icon_theme_get_default();
    pb = gtk_icon_theme_load_icon(theme, icon_name, 16, GTK_ICON_LOOKUP_FORCE_SIZE, NULL);
    if (pb == NULL && kind == DISKATLAS_NODE_KIND_SYMLINK) {
      pb = gtk_icon_theme_load_icon(theme, "text-x-generic", 16, GTK_ICON_LOOKUP_FORCE_SIZE, NULL);
    }
  }

  g_free(path_utf8);
  g_object_set(GTK_CELL_RENDERER_PIXBUF(cell), "pixbuf", pb, NULL);
  if (pb != NULL) {
    g_object_unref(pb);
  }
}

static void tv_pct_of_parent_cell_data(GtkTreeViewColumn *column, GtkCellRenderer *cell,
                                       GtkTreeModel *model, GtkTreeIter *iter,
                                       gpointer user_data) {
  (void)user_data;
  gint pv = -1;
  gchar *txt = NULL;
  gtk_tree_model_get(model, iter, DA_TV_COL_PCT_VAL, &pv, DA_TV_COL_PCT_LABEL, &txt, -1);
  gint v = (pv >= 0) ? (pv > 100 ? 100 : pv) : 0;

  gint col_w = gtk_tree_view_column_get_width(column);
  if (col_w <= 1) {
    col_w = gtk_tree_view_column_get_fixed_width(column);
  }
  if (col_w > 4) {
    gtk_cell_renderer_set_fixed_size(cell, col_w - 4, -1);
  } else {
    gtk_cell_renderer_set_fixed_size(cell, -1, -1);
  }

  g_object_set(GTK_CELL_RENDERER_PROGRESS(cell), "value", v,
               "text", txt != NULL ? txt : "", "text-xalign", 0.98f, NULL);
  g_free(txt);
}

static void append_tv_pct_column(GtkTreeView *tv, const char *title, int width_px, int min_width_px) {
  GtkCellRenderer *r = da_cell_renderer_progress_new();
  g_object_set(r, "xpad", 0, "ypad", 0, "xalign", 0.0f, NULL);
  GtkTreeViewColumn *c = gtk_tree_view_column_new();
  gtk_tree_view_column_set_title(c, title);
  gtk_tree_view_column_pack_start(c, r, TRUE);
  gtk_tree_view_column_set_cell_data_func(c, r, tv_pct_of_parent_cell_data, NULL, NULL);
  gtk_tree_view_column_set_alignment(c, 1.0f);
  gtk_tree_view_column_set_resizable(c, TRUE);
  gtk_tree_view_column_set_sizing(c, GTK_TREE_VIEW_COLUMN_FIXED);
  gtk_tree_view_column_set_min_width(c, min_width_px);
  gtk_tree_view_column_set_fixed_width(c, width_px);
  gtk_tree_view_column_set_sort_column_id(c, DA_TV_COL_PCT_VAL);
  gtk_tree_view_append_column(tv, c);
}

static void da_setup_tree_view(AppState *app) {
  if (app->tree_view == NULL) {
    return;
  }

  app->tree_view_store = da_tree_view_store_new();
  gtk_tree_view_set_model(GTK_TREE_VIEW(app->tree_view), GTK_TREE_MODEL(app->tree_view_store));
  g_object_unref(app->tree_view_store);

  gtk_tree_selection_set_mode(gtk_tree_view_get_selection(GTK_TREE_VIEW(app->tree_view)),
                              GTK_SELECTION_MULTIPLE);

  gtk_tree_view_set_fixed_height_mode(GTK_TREE_VIEW(app->tree_view), TRUE);

  /* Column 0: Folder (icon + text with tree expander). */
  {
    GtkCellRenderer *pix = gtk_cell_renderer_pixbuf_new();
    GtkCellRenderer *txt = gtk_cell_renderer_text_new();
    g_object_set(txt, "ellipsize", PANGO_ELLIPSIZE_END, "xalign", 0.0f, NULL);
    GtkTreeViewColumn *c = gtk_tree_view_column_new();
    gtk_tree_view_column_set_title(c, "Folder");
    gtk_tree_view_column_pack_start(c, pix, FALSE);
    gtk_tree_view_column_set_cell_data_func(c, pix, tv_icon_cell_data, NULL, NULL);
    gtk_tree_view_column_pack_start(c, txt, TRUE);
    gtk_tree_view_column_add_attribute(c, txt, "text", DA_TV_COL_NAME);
    gtk_tree_view_column_set_alignment(c, 0.0f);
    gtk_tree_view_column_set_resizable(c, TRUE);
    gtk_tree_view_column_set_sizing(c, GTK_TREE_VIEW_COLUMN_FIXED);
    gtk_tree_view_column_set_min_width(c, 140);
    gtk_tree_view_column_set_fixed_width(c, 300);
    gtk_tree_view_column_set_sort_column_id(c, DA_TV_COL_NAME);
    gtk_tree_view_append_column(GTK_TREE_VIEW(app->tree_view), c);
  }

  /* Column 1: Percent of Parent (progress bar). */
  append_tv_pct_column(GTK_TREE_VIEW(app->tree_view), "Percent of Parent", 130, 88);

  /* Remaining text columns: Size, Allocated, Items, Files, Folders, Modified, Attributes. */
  static const char  *tv_titles[] = { "Size", "Allocated", "Items", "Files", "Folders", "Modified", "Attributes" };
  static const int    tv_cols[]   = { DA_TV_COL_SIZE, DA_TV_COL_ALLOC, DA_TV_COL_ITEMS,
                                      DA_TV_COL_FILES, DA_TV_COL_FOLDERS, DA_TV_COL_MODIFIED, DA_TV_COL_ATTRS };
  static const int    tv_widths[] = { 100, 100, 80, 80, 80, 140, 68 };
  static const int    tv_minw[]   = { 72, 72, 56, 56, 56, 100, 48 };
  static const gfloat tv_align[]  = { 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 0.0f, 0.0f };
  for (int i = 0; i < 7; i++) {
    const char *cell_bg = tv_cols[i] == DA_TV_COL_ALLOC ? DISKATLAS_TREE_ALLOC_CELL_BG : NULL;
    append_text_column(GTK_TREE_VIEW(app->tree_view), tv_titles[i], tv_cols[i], tv_cols[i],
                       tv_widths[i], tv_minw[i], tv_align[i], cell_bg);
  }
}

void da_ui_build(AppState *app) {
  da_scan_source_install_gio_log_filter_once();
  da_load_global_app_css();

  GtkBuilder *builder = gtk_builder_new();
  GError *err = NULL;
  if (!gtk_builder_add_from_resource(builder, DISKATLAS_WINDOW_UI_RESOURCE, &err)) {
    g_error("Failed to load UI (%s): %s", DISKATLAS_WINDOW_UI_RESOURCE, err->message);
  }

  app->window = GTK_WIDGET(gtk_builder_get_object(builder, "main_window"));
  gtk_window_set_application(GTK_WINDOW(app->window), app->gtk_app);

  app->scan_source_combo = GTK_WIDGET(gtk_builder_get_object(builder, "scan_source_combo"));

  da_scan_source_combo_setup(app);

  app->scan_btn = GTK_WIDGET(gtk_builder_get_object(builder, "scan_btn"));
  app->panel_scan_label = GTK_WIDGET(gtk_builder_get_object(builder, "panel_scan_label"));
  app->progress = GTK_WIDGET(gtk_builder_get_object(builder, "progress"));
  app->search = GTK_WIDGET(gtk_builder_get_object(builder, "search"));
  app->duplicates_file_combo = GTK_WIDGET(gtk_builder_get_object(builder, "duplicates_file_combo"));
  app->match_filename_only_radio = GTK_WIDGET(gtk_builder_get_object(builder, "match_filename_only_radio"));
  app->match_entire_path_radio = GTK_WIDGET(gtk_builder_get_object(builder, "match_entire_path_radio"));
  app->duplicates_only_check = GTK_WIDGET(gtk_builder_get_object(builder, "duplicates_only_check"));
  app->show_folders_check = GTK_WIDGET(gtk_builder_get_object(builder, "show_folders_check"));
  app->combo_display_max = GTK_WIDGET(gtk_builder_get_object(builder, "combo_display_max"));
  app->tree = GTK_WIDGET(gtk_builder_get_object(builder, "file_view_tree"));
  gtk_style_context_add_class(gtk_widget_get_style_context(app->tree), DISKATLAS_TREE_PROGRESS_STYLE_CLASS);
  gtk_widget_add_events(app->tree, GDK_POINTER_MOTION_MASK | GDK_LEAVE_NOTIFY_MASK);
  app->treemap_panel_title = GTK_WIDGET(gtk_builder_get_object(builder, "treemap_panel_title"));
  app->main_notebook = GTK_WIDGET(gtk_builder_get_object(builder, "main_notebook"));
  app->tree_view = GTK_WIDGET(gtk_builder_get_object(builder, "tree_view_tree"));
  gtk_style_context_add_class(gtk_widget_get_style_context(app->tree_view), DISKATLAS_TREE_PROGRESS_STYLE_CLASS);
  {
    GtkWidget *tv_scrolled = GTK_WIDGET(gtk_builder_get_object(builder, "tree_view_scrolled"));
    GtkWidget *tv_paned = GTK_WIDGET(gtk_builder_get_object(builder, "tree_view_paned"));
    if (tv_scrolled != NULL) {
      gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(tv_scrolled), GTK_POLICY_AUTOMATIC,
                                     GTK_POLICY_AUTOMATIC);
      gtk_widget_set_valign(tv_scrolled, GTK_ALIGN_FILL);
      g_signal_connect(tv_scrolled, "size-allocate", G_CALLBACK(da_tree_scrolled_on_allocate), NULL);
    }
    if (tv_paned != NULL && GTK_IS_PANED(tv_paned) && tv_scrolled != NULL) {
      g_signal_connect(tv_paned, "notify::position", G_CALLBACK(da_file_view_paned_on_notify_position),
                       tv_scrolled);
      g_signal_connect(tv_paned, "size-allocate", G_CALLBACK(da_file_view_paned_on_allocate), tv_scrolled);
    }
  }
  app->status_label_left = GTK_WIDGET(gtk_builder_get_object(builder, "status_label_left"));
  app->status_label_center = GTK_WIDGET(gtk_builder_get_object(builder, "status_label_center"));
  app->status_label_right = GTK_WIDGET(gtk_builder_get_object(builder, "status_label_right"));

  app->stat_sel_val = GTK_WIDGET(gtk_builder_get_object(builder, "stat_sel_val"));
  app->stat_tot_val = GTK_WIDGET(gtk_builder_get_object(builder, "stat_tot_val"));
  app->stat_use_val = GTK_WIDGET(gtk_builder_get_object(builder, "stat_use_val"));
  app->stat_free_val = GTK_WIDGET(gtk_builder_get_object(builder, "stat_free_val"));

  GtkWidget *tree_scrolled = GTK_WIDGET(gtk_builder_get_object(builder, "file_view_scrolled"));
  gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(tree_scrolled), GTK_POLICY_AUTOMATIC, GTK_POLICY_AUTOMATIC);
  gtk_widget_set_valign(tree_scrolled, GTK_ALIGN_FILL);
  g_signal_connect(tree_scrolled, "size-allocate", G_CALLBACK(da_tree_scrolled_on_allocate), NULL);

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

  app->flat_list_model = flat_list_model_new(app);
  gtk_tree_view_set_model(GTK_TREE_VIEW(app->tree), GTK_TREE_MODEL(app->flat_list_model));
  gtk_tree_selection_set_mode(gtk_tree_view_get_selection(GTK_TREE_VIEW(app->tree)),
                              GTK_SELECTION_MULTIPLE);
  g_object_unref(app->flat_list_model);

  const char *titles[] = {"File Name", "Path", "% of used space", "Size", "Allocated", "Modified",
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
    const char *cell_bg = (i == DA_COL_ALLOCATED) ? DISKATLAS_TREE_ALLOC_CELL_BG : NULL;
    append_text_column(GTK_TREE_VIEW(app->tree), titles[i], i, col_sort_id[i], col_w[i], col_min_w[i], xalign,
                       cell_bg);
  }

  /* Sorting is handled internally by FlatListModel via GtkTreeSortable.
   * fixed_height_mode is safe with children as long as all rows (parent and
   * child) use the same cell renderers at the same font size — which they do. */
  gtk_tree_view_set_fixed_height_mode(GTK_TREE_VIEW(app->tree), TRUE);

  da_setup_tree_view(app);

#if defined(G_OS_WIN32)
  app->admin_ntfs_notice_panel = GTK_WIDGET(gtk_builder_get_object(builder, "admin_ntfs_notice_panel"));
  app->restart_admin_btn = GTK_WIDGET(gtk_builder_get_object(builder, "restart_admin_btn"));
  app->dont_show_again_check = GTK_WIDGET(gtk_builder_get_object(builder, "dont_show_again_check"));
  if (app->restart_admin_btn != NULL) {
    g_signal_connect(app->restart_admin_btn, "clicked", G_CALLBACK(on_restart_admin_clicked), app);
  }
  if (app->dont_show_again_check != NULL) {
    g_signal_connect(app->dont_show_again_check, "toggled", G_CALLBACK(on_dont_show_admin_ntfs_toggled),
                     app);
  }
#else
  {
    GtkWidget *admin_panel = GTK_WIDGET(gtk_builder_get_object(builder, "admin_ntfs_notice_panel"));
    if (admin_panel != NULL) {
      gtk_widget_hide(admin_panel);
    }
  }
  app->admin_ntfs_notice_panel = NULL;
  app->restart_admin_btn = NULL;
  app->dont_show_again_check = NULL;
#endif

  g_object_unref(builder);

  scan_controller_sync_display_max_combo(app);
  scan_controller_attach(app);
  scan_controller_refresh_volume_labels(app);

  gtk_widget_show_all(app->window);

#if defined(G_OS_WIN32)
  /* gtk_widget_show_all reveals hidden descendants — re-apply admin banner visibility last. */
  if (da_win32_is_process_elevated() || da_win32_admin_ntfs_notice_saved_hidden()) {
    if (app->admin_ntfs_notice_panel != NULL) {
      gtk_widget_hide(app->admin_ntfs_notice_panel);
    }
  } else if (app->admin_ntfs_notice_panel != NULL) {
    gtk_widget_show(app->admin_ntfs_notice_panel);
  }
#endif
}
