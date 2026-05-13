#include <stdlib.h>
#include <stdint.h>
#include <string.h>

#include <glib.h>
#include <gdk/gdk.h>
#include <gtk/gtk.h>

#include "diskatlas.h"
#include "dm_treemap_colors.h"

#include "treemap_widget.h"
#include "file_tree_model.h"
#include "tree_view_model.h"
#include "scan_controller.h"
#include "scan_source_combo.h"
#include "file_ops.h"
#include "csv_export.h"
#include "ui_window.h"
#include "volumes.h"
#include "diskatlas_ini.h"
#include "dm_mime_db.h"
#include "settings_mime_tab.h"
#include "file_type_view.h"
#include "da_cell_renderer_progress.h"
#include "flat_list_model.h"
#include "format_text.h"
#include "shell_icon.h"

void da_ui_sync_file_menu(AppState *app) {
  gboolean ok = FALSE;
  if (app != NULL && app->scan != NULL) {
    scan_progress_t pr = scan_get_progress(app->scan);
    if (pr.is_complete) {
      scan_results_view_t v = scan_get_results(app->scan);
      ok = (v.nodes != NULL);
    }
  }
  if (app != NULL && app->file_menu_export_csv != NULL) {
    gtk_widget_set_sensitive(app->file_menu_export_csv, ok);
  }
  if (app != NULL && app->file_menu_copy_clipboard != NULL) {
    gtk_widget_set_sensitive(app->file_menu_copy_clipboard, ok);
  }
  gboolean ok_sel = ok && scan_controller_file_menu_selection_commands_sensitive(app);
  if (app != NULL && app->file_menu_explore_folder != NULL) {
    gtk_widget_set_sensitive(app->file_menu_explore_folder, ok_sel);
  }
  if (app != NULL && app->file_menu_terminal != NULL) {
    gtk_widget_set_sensitive(app->file_menu_terminal, ok_sel);
  }
  if (app != NULL && app->file_menu_copy_path != NULL) {
    gtk_widget_set_sensitive(app->file_menu_copy_path, ok_sel);
  }
  if (app != NULL && app->file_menu_copy != NULL) {
    gtk_widget_set_sensitive(app->file_menu_copy, ok_sel);
  }
  if (app != NULL && app->file_menu_cut != NULL) {
    gtk_widget_set_sensitive(app->file_menu_cut, ok_sel);
  }
  if (app != NULL && app->file_menu_delete_trash != NULL) {
    gtk_widget_set_sensitive(app->file_menu_delete_trash, ok_sel);
  }
  if (app != NULL && app->file_menu_delete_permanent != NULL) {
    gtk_widget_set_sensitive(app->file_menu_delete_permanent, ok_sel);
  }
  gboolean ok_rename = ok_sel && app != NULL && app->general_enable_rename;
  if (app != NULL && app->file_menu_rename != NULL) {
    gtk_widget_set_sensitive(app->file_menu_rename, ok_rename);
  }
  /* Zoom In: scan complete AND at least one treemap tile is selected. */
  gboolean ok_zoom_in = ok && app != NULL &&
                        app->treemap != NULL && TREEMAP_IS_WIDGET(app->treemap) &&
                        treemap_widget_has_selection(TREEMAP_WIDGET(app->treemap));
  if (app != NULL && app->file_menu_zoom_in != NULL) {
    gtk_widget_set_sensitive(app->file_menu_zoom_in, ok_zoom_in);
  }
  /* Zoom Out: scan complete AND we are currently zoomed into a sub-folder. */
  gboolean ok_zoom_out = ok && app != NULL &&
                         app->treemap_zoom_root_utf8 != NULL &&
                         app->treemap_zoom_root_utf8[0] != '\0';
  if (app != NULL && app->file_menu_zoom_out != NULL) {
    gtk_widget_set_sensitive(app->file_menu_zoom_out, ok_zoom_out);
  }

  /* Context menu: mirror the same sensitivity rules. */
  if (app != NULL && app->context_menu_export_csv != NULL) {
    gtk_widget_set_sensitive(app->context_menu_export_csv, ok);
  }
  if (app != NULL && app->context_menu_copy_file_info != NULL) {
    gtk_widget_set_sensitive(app->context_menu_copy_file_info, ok);
  }
  if (app != NULL && app->context_menu_explore_folder != NULL) {
    gtk_widget_set_sensitive(app->context_menu_explore_folder, ok_sel);
  }
  if (app != NULL && app->context_menu_terminal_here != NULL) {
    gtk_widget_set_sensitive(app->context_menu_terminal_here, ok_sel);
  }
  if (app != NULL && app->context_menu_copy_path != NULL) {
    gtk_widget_set_sensitive(app->context_menu_copy_path, ok_sel);
  }
  if (app != NULL && app->context_menu_zoom_in != NULL) {
    gtk_widget_set_sensitive(app->context_menu_zoom_in, ok_zoom_in);
  }
  if (app != NULL && app->context_menu_zoom_out != NULL) {
    gtk_widget_set_sensitive(app->context_menu_zoom_out, ok_zoom_out);
  }
}

#define DA_SIZE_FMT_MENU_DATA_KEY "da-size-fmt"

static void on_size_display_format_activate(GtkMenuItem *item, gpointer user_data) {
  AppState *app = (AppState *)user_data;
  if (app == NULL || item == NULL || !GTK_IS_CHECK_MENU_ITEM(item)) {
    return;
  }
  if (!gtk_check_menu_item_get_active(GTK_CHECK_MENU_ITEM(item))) {
    return;
  }
  gint fmt = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(item), DA_SIZE_FMT_MENU_DATA_KEY));
  scan_controller_set_size_display_format(app, fmt);
}

static void da_ui_apply_header_panel_visibility(AppState *app) {
  if (app == NULL || app->header_panel == NULL) {
    return;
  }
  gtk_widget_set_visible(app->header_panel, app->interface_show_header);
}

static void da_ui_apply_tree_view_tab_extras_visibility(AppState *app) {
  if (app == NULL) {
    return;
  }
  if (app->file_type_scrolled != NULL) {
    gtk_widget_set_visible(app->file_type_scrolled, app->interface_show_file_types);
  }
  if (app->treemap_panel != NULL) {
    gtk_widget_set_visible(app->treemap_panel, app->interface_show_treemap);
  }
}

static void on_options_menu_show_header_toggled(GtkCheckMenuItem *item, gpointer user_data) {
  AppState *app = (AppState *)user_data;
  if (app == NULL || item == NULL) {
    return;
  }
  app->interface_show_header = gtk_check_menu_item_get_active(item);
  da_ui_apply_header_panel_visibility(app);
  da_ini_save_interface(app);
}

static void on_options_menu_show_file_types_toggled(GtkCheckMenuItem *item, gpointer user_data) {
  AppState *app = (AppState *)user_data;
  if (app == NULL || item == NULL) {
    return;
  }
  app->interface_show_file_types = gtk_check_menu_item_get_active(item);
  da_ui_apply_tree_view_tab_extras_visibility(app);
  da_ini_save_interface(app);
}

static void on_options_menu_show_treemap_toggled(GtkCheckMenuItem *item, gpointer user_data) {
  AppState *app = (AppState *)user_data;
  if (app == NULL || item == NULL) {
    return;
  }
  app->interface_show_treemap = gtk_check_menu_item_get_active(item);
  da_ui_apply_tree_view_tab_extras_visibility(app);
  da_ini_save_interface(app);
}

static void on_options_menu_show_free_space_toggled(GtkCheckMenuItem *item, gpointer user_data) {
  AppState *app = (AppState *)user_data;
  if (app == NULL || item == NULL) {
    return;
  }
  app->interface_treemap_show_free_space = gtk_check_menu_item_get_active(item);
  if (app->treemap != NULL && TREEMAP_IS_WIDGET(app->treemap)) {
    const char *root_lbl = (app->treemap_zoom_root_utf8 != NULL && app->treemap_zoom_root_utf8[0] != '\0')
                             ? app->treemap_zoom_root_utf8
                             : (app->scan_root_utf8 != NULL && app->scan_root_utf8[0] != '\0')
                               ? app->scan_root_utf8
                               : app->csv_derived_root_utf8;
    treemap_widget_set_free_space(TREEMAP_WIDGET(app->treemap),
                                  app->interface_treemap_show_free_space,
                                  app->volume_free_bytes,
                                  (app->volume_total_bytes > app->volume_free_bytes)
                                    ? app->volume_total_bytes - app->volume_free_bytes
                                    : 0u,
                                  root_lbl);
  }
  da_ini_save_interface(app);
}

static void on_options_menu_show_free_labels_toggled(GtkCheckMenuItem *item, gpointer user_data) {
  AppState *app = (AppState *)user_data;
  if (app == NULL || item == NULL) {
    return;
  }
  app->interface_treemap_show_labels = gtk_check_menu_item_get_active(item);
  if (app->treemap != NULL && TREEMAP_IS_WIDGET(app->treemap)) {
    treemap_widget_set_show_labels(TREEMAP_WIDGET(app->treemap), app->interface_treemap_show_labels);
  }
  da_ini_save_interface(app);
}

static gboolean on_widget_right_click(GtkWidget *w, GdkEventButton *ev, gpointer user_data) {
  (void)w;
  AppState *app = (AppState *)user_data;
  if (ev->button != 3 || ev->type != GDK_BUTTON_PRESS || app == NULL || app->context_menu == NULL) {
    return FALSE;
  }
  da_ui_sync_file_menu(app);
  gtk_menu_popup_at_pointer(GTK_MENU(app->context_menu), (GdkEvent *)ev);
  return TRUE;
}

static void on_file_menu_zoom_in_activate(GtkMenuItem *item, gpointer user_data) {
  (void)item;
  scan_controller_treemap_zoom_in((AppState *)user_data);
}

static void on_file_menu_zoom_out_activate(GtkMenuItem *item, gpointer user_data) {
  (void)item;
  scan_controller_treemap_zoom_out((AppState *)user_data);
}

/* ---- PNG export dialog ---------------------------------------------------- */

static void on_file_menu_treemap_image_activate(GtkMenuItem *item, gpointer user_data) {
  AppState *app = (AppState *)user_data;
  (void)item;
  if (app == NULL || app->treemap == NULL || !TREEMAP_IS_WIDGET(app->treemap)) {
    return;
  }

  /* Step 1: file chooser for output path. */
  GtkWidget *chooser = gtk_file_chooser_dialog_new(
      "Save Treemap as PNG",
      GTK_WINDOW(app->window),
      GTK_FILE_CHOOSER_ACTION_SAVE,
      "_Cancel", GTK_RESPONSE_CANCEL,
      "_Save",   GTK_RESPONSE_ACCEPT,
      NULL);
  gtk_file_chooser_set_do_overwrite_confirmation(GTK_FILE_CHOOSER(chooser), TRUE);
  gtk_file_chooser_set_current_name(GTK_FILE_CHOOSER(chooser), "treemap.png");

  GtkFileFilter *ff = gtk_file_filter_new();
  gtk_file_filter_set_name(ff, "PNG images (*.png)");
  gtk_file_filter_add_pattern(ff, "*.png");
  gtk_file_chooser_add_filter(GTK_FILE_CHOOSER(chooser), ff);

  if (gtk_dialog_run(GTK_DIALOG(chooser)) != GTK_RESPONSE_ACCEPT) {
    gtk_widget_destroy(chooser);
    return;
  }
  gchar *output_path = gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(chooser));
  gtk_widget_destroy(chooser);
  if (output_path == NULL) {
    return;
  }

  /* Step 2: options dialog (width, height, grayscale, free space). */
  GtkWidget *dlg = gtk_dialog_new_with_buttons(
      "Export Options",
      GTK_WINDOW(app->window),
      GTK_DIALOG_MODAL | GTK_DIALOG_DESTROY_WITH_PARENT,
      "_Cancel", GTK_RESPONSE_CANCEL,
      "_Export", GTK_RESPONSE_OK,
      NULL);
  GtkWidget *content = gtk_dialog_get_content_area(GTK_DIALOG(dlg));
  gtk_container_set_border_width(GTK_CONTAINER(content), 12);
  gtk_box_set_spacing(GTK_BOX(content), 6);

  /* Width */
  GtkWidget *hb_w = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
  gtk_box_pack_start(GTK_BOX(hb_w), gtk_label_new("Width:"), FALSE, FALSE, 0);
  GtkWidget *spin_w = gtk_spin_button_new_with_range(100, 16384, 1);
  gtk_spin_button_set_value(GTK_SPIN_BUTTON(spin_w), 1920);
  gtk_box_pack_start(GTK_BOX(hb_w), spin_w, FALSE, FALSE, 0);
  gtk_box_pack_start(GTK_BOX(content), hb_w, FALSE, FALSE, 0);

  /* Height */
  GtkWidget *hb_h = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
  gtk_box_pack_start(GTK_BOX(hb_h), gtk_label_new("Height:"), FALSE, FALSE, 0);
  GtkWidget *spin_h = gtk_spin_button_new_with_range(100, 16384, 1);
  gtk_spin_button_set_value(GTK_SPIN_BUTTON(spin_h), 1080);
  gtk_box_pack_start(GTK_BOX(hb_h), spin_h, FALSE, FALSE, 0);
  gtk_box_pack_start(GTK_BOX(content), hb_h, FALSE, FALSE, 0);

  /* Grayscale */
  GtkWidget *chk_gray = gtk_check_button_new_with_label("Grayscale (shades of gray instead of color)");
  gtk_box_pack_start(GTK_BOX(content), chk_gray, FALSE, FALSE, 0);

  /* Show free space */
  GtkWidget *chk_free = gtk_check_button_new_with_label("Show free space tile");
  gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(chk_free), app->interface_treemap_show_free_space);
  gtk_box_pack_start(GTK_BOX(content), chk_free, FALSE, FALSE, 0);

  gtk_widget_show_all(content);

  if (gtk_dialog_run(GTK_DIALOG(dlg)) != GTK_RESPONSE_OK) {
    gtk_widget_destroy(dlg);
    g_free(output_path);
    return;
  }

  int exp_w  = (int)gtk_spin_button_get_value(GTK_SPIN_BUTTON(spin_w));
  int exp_h  = (int)gtk_spin_button_get_value(GTK_SPIN_BUTTON(spin_h));
  gboolean gray      = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(chk_gray));
  gboolean show_free = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(chk_free));
  gtk_widget_destroy(dlg);

  uint64_t used_b = (app->volume_total_bytes > app->volume_free_bytes)
                    ? app->volume_total_bytes - app->volume_free_bytes : 0u;

  gboolean ok = treemap_widget_export_png(TREEMAP_WIDGET(app->treemap),
                                          output_path, exp_w, exp_h,
                                          gray, show_free,
                                          app->volume_free_bytes, used_b);
  if (!ok) {
    GtkWidget *err = gtk_message_dialog_new(GTK_WINDOW(app->window),
                                            GTK_DIALOG_MODAL,
                                            GTK_MESSAGE_ERROR,
                                            GTK_BUTTONS_OK,
                                            "Failed to export treemap PNG.\n"
                                            "Make sure the treemap is visible and a scan is loaded.");
    gtk_dialog_run(GTK_DIALOG(err));
    gtk_widget_destroy(err);
  }
  g_free(output_path);
}

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
#define DISKATLAS_SETTINGS_DIALOG_RESOURCE "/ui/settings_dialog.glade"
#define DISKATLAS_APP_CSS_RESOURCE "/app.css"
/** gtk_style_context_add_class for percent-column progress CSS (file list + Tree View tabs). */
#define DISKATLAS_TREE_PROGRESS_STYLE_CLASS "diskatlas-tree-progress"
/** Default GtkCellRendererText background for Allocated columns (file list + Tree View tab). */
#define DISKATLAS_TREE_ALLOC_CELL_BG "#EFEFEF"

/** GtkCellRendererText: `background`; pixbuf/progress: `cell-background` (GTK 3). */
static void da_cell_renderer_apply_row_bg(GtkCellRenderer *cell, const gchar *html, gboolean set) {
  if (cell == NULL) {
    return;
  }
  if (GTK_IS_CELL_RENDERER_TEXT(cell)) {
    if (!set) {
      g_object_set(cell, "background", NULL, "background-set", FALSE, NULL);
    } else {
      g_object_set(cell, "background", html, "background-set", TRUE, NULL);
    }
    return;
  }
  if (GTK_IS_CELL_RENDERER_PIXBUF(cell) || GTK_IS_CELL_RENDERER_PROGRESS(cell)) {
    if (!set) {
      g_object_set(cell, "cell-background", NULL, "cell-background-set", FALSE, NULL);
    } else {
      g_object_set(cell, "cell-background", html, "cell-background-set", TRUE, NULL);
    }
  }
}

void da_tree_view_apply_zebra_cell(GtkTreeViewColumn *col, GtkCellRenderer *cell, GtkTreeModel *model,
                                   GtkTreeIter *iter, AppState *app, gboolean column_is_allocated) {
  if (cell == NULL || col == NULL || model == NULL || iter == NULL) {
    return;
  }
  GtkTreeView *tv = GTK_TREE_VIEW(gtk_tree_view_column_get_tree_view(col));
  if (tv == NULL) {
    return;
  }
  GtkTreeSelection *sel = gtk_tree_view_get_selection(tv);
  if (gtk_tree_selection_iter_is_selected(sel, iter)) {
    da_cell_renderer_apply_row_bg(cell, NULL, FALSE);
    return;
  }
  if (column_is_allocated) {
    da_cell_renderer_apply_row_bg(cell, DISKATLAS_TREE_ALLOC_CELL_BG, TRUE);
    return;
  }
  if (app == NULL || !app->interface_alternate_row_colors) {
    da_cell_renderer_apply_row_bg(cell, NULL, FALSE);
    return;
  }

  GtkTreePath *path = gtk_tree_model_get_path(model, iter);
  if (path == NULL) {
    da_cell_renderer_apply_row_bg(cell, NULL, FALSE);
    return;
  }

  GdkRectangle rect;
  memset(&rect, 0, sizeof(rect));
  gtk_tree_view_get_background_area(tv, path, NULL, &rect);
  gboolean stripe = FALSE;
  if (rect.height > 0) {
    gint band = rect.y / rect.height;
    stripe = (band % 2) == 0;
  } else {
    gint *inds = gtk_tree_path_get_indices(path);
    gint depth = gtk_tree_path_get_depth(path);
    if (inds != NULL && depth >= 1) {
      stripe = ((inds[depth - 1] % 2) == 0);
    }
  }
  gtk_tree_path_free(path);

  if (stripe) {
    da_cell_renderer_apply_row_bg(cell, DISKATLAS_TREE_ALLOC_CELL_BG, TRUE);
  } else {
    da_cell_renderer_apply_row_bg(cell, NULL, FALSE);
  }
}

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

/* ---- Deleted-state cell styling helpers ---- */

/**
 * Apply red foreground + strikethrough to a text cell renderer when the given
 * path is in app->deleted_path_set; otherwise reset those properties.
 */
static void da_apply_deleted_cell_style(GtkCellRenderer *cell, AppState *app, const char *path) {
  gboolean is_deleted = FALSE;
  if (app != NULL && app->deleted_path_set != NULL && path != NULL && path[0] != '\0') {
    is_deleted = g_hash_table_contains(app->deleted_path_set, path);
  }
  if (GTK_IS_CELL_RENDERER_TEXT(cell)) {
    g_object_set(cell,
                 "foreground",        is_deleted ? "red" : NULL,
                 "foreground-set",    is_deleted,
                 "strikethrough",     is_deleted,
                 "strikethrough-set", is_deleted,
                 NULL);
  } else if (DA_IS_CELL_RENDERER_PROGRESS(cell)) {
    g_object_set(cell, "strikethrough", is_deleted, NULL);
  }
}

/**
 * Cell data function context for a plain text column in either tree view.
 * Stores: the AppState pointer, the model column index for the text value,
 * and whether this is a tree view row (uses DA_TV_COL_PATH for lookup) or a
 * file view row (uses DA_COL_LP for lookup).
 */
typedef struct {
  AppState *app;
  gint      model_text_col;  /* model column that holds the display string */
  gboolean  is_tree_view;    /* TRUE = tree_view_tree, FALSE = file_view_tree */
} DaDeletedCellCtx;

/* File view: look up nid from DA_COL_LP and get path from scan results. */
static void da_fv_deleted_text_cell_data(GtkTreeViewColumn *col, GtkCellRenderer *cell,
                                          GtkTreeModel *model, GtkTreeIter *iter,
                                          gpointer user_data) {
  DaDeletedCellCtx *ctx = (DaDeletedCellCtx *)user_data;
  if (ctx == NULL) {
    return;
  }
  /* Set text from the bound model column. */
  gchar *text = NULL;
  gtk_tree_model_get(model, iter, ctx->model_text_col, &text, -1);
  g_object_set(cell, "text", text, NULL);
  g_free(text);

  /* Deleted styling: resolve nid → path → check deleted set. */
  const char *path = NULL;
  gchar *path_buf = NULL;
  AppState *app = ctx->app;
  if (app != NULL && app->deleted_path_set != NULL && app->scan != NULL) {
    gint64 lp = 0;
    gtk_tree_model_get(model, iter, DA_COL_LP, &lp, -1);
    size_t nid = SIZE_MAX;
    if (da_tree_lp_to_scan_nid(app, lp, &nid) && nid != SIZE_MAX) {
      scan_results_view_t v = scan_get_results(app->scan);
      if (v.nodes != NULL && nid < v.count) {
        path = v.nodes[nid].path;
      }
    }
    (void)path_buf;
  }
  da_apply_deleted_cell_style(cell, app, path);

  gboolean is_alloc = !ctx->is_tree_view && (ctx->model_text_col == DA_COL_ALLOCATED);
  da_tree_view_apply_zebra_cell(col, cell, model, iter, app, is_alloc);
}

/* Tree view: read DA_TV_COL_PATH to find deleted state. */
static void da_tv_deleted_text_cell_data(GtkTreeViewColumn *col, GtkCellRenderer *cell,
                                          GtkTreeModel *model, GtkTreeIter *iter,
                                          gpointer user_data) {
  DaDeletedCellCtx *ctx = (DaDeletedCellCtx *)user_data;
  if (ctx == NULL) {
    return;
  }
  /* Set text from the bound model column. */
  gchar *text = NULL;
  gtk_tree_model_get(model, iter, ctx->model_text_col, &text, -1);
  g_object_set(cell, "text", text, NULL);
  g_free(text);

  /* Deleted styling: read path from DA_TV_COL_PATH. */
  gchar *path = NULL;
  gtk_tree_model_get(model, iter, DA_TV_COL_PATH, &path, -1);
  da_apply_deleted_cell_style(cell, ctx->app, path);
  g_free(path);

  gboolean is_alloc = ctx->is_tree_view && (ctx->model_text_col == DA_TV_COL_ALLOC);
  da_tree_view_apply_zebra_cell(col, cell, model, iter, ctx->app, is_alloc);
  gboolean name_editable = ctx->app != NULL && ctx->app->general_enable_rename && ctx->is_tree_view &&
                           ctx->model_text_col == DA_TV_COL_NAME;
  g_object_set(cell, "editable", name_editable, NULL);
}

/**
 * Install a deleted-state cell data function on the first
 * GtkCellRendererText in the given column of @a tv.
 * @param tv         GtkTreeView.
 * @param col_index  Zero-based view column index.
 * @param app        AppState pointer.
 * @param model_text_col  The model column that holds the text value.
 * @param is_tv      TRUE for tree_view_tree (uses DA_TV_COL_PATH), FALSE for file_view_tree.
 */
static void da_install_deleted_text_style(GtkTreeView *tv, gint col_index, AppState *app,
                                           gint model_text_col, gboolean is_tv) {
  GtkTreeViewColumn *col = gtk_tree_view_get_column(tv, col_index);
  if (col == NULL) {
    return;
  }
  GList *cells = gtk_cell_layout_get_cells(GTK_CELL_LAYOUT(col));
  for (GList *l = cells; l != NULL; l = l->next) {
    GtkCellRenderer *r = GTK_CELL_RENDERER(l->data);
    if (!GTK_IS_CELL_RENDERER_TEXT(r) || GTK_IS_CELL_RENDERER_PROGRESS(r)) {
      continue;
    }
    DaDeletedCellCtx *ctx = g_new(DaDeletedCellCtx, 1);
    ctx->app            = app;
    ctx->model_text_col = model_text_col;
    ctx->is_tree_view   = is_tv;
    GtkTreeCellDataFunc fn = is_tv ? da_tv_deleted_text_cell_data : da_fv_deleted_text_cell_data;
    gtk_tree_view_column_set_cell_data_func(col, r, fn, ctx, g_free);
    break; /* Only the first text renderer per column. */
  }
  g_list_free(cells);
}

static void file_name_icon_cell_data(GtkTreeViewColumn *column, GtkCellRenderer *cell, GtkTreeModel *model,
                                     GtkTreeIter *iter, gpointer user_data) {
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
  da_tree_view_apply_zebra_cell(column, cell, model, iter, app, FALSE);
}

static void file_view_set_search_highlight_cell(GtkCellRenderer *cell, AppState *app, const gchar *plain) {
  const gchar *s = plain != NULL ? plain : "";
  if (app != NULL && app->filter_active && app->filter_text[0] != '\0') {
    gchar *m = da_search_filter_markup(s, app->filter_text);
    if (m != NULL) {
      g_object_set(cell, "markup", m, NULL);
      g_free(m);
      return;
    }
  }
  g_object_set(cell, "text", s, NULL);
}

static void file_view_file_name_text_cell_data(GtkTreeViewColumn *column, GtkCellRenderer *cell,
                                                GtkTreeModel *model, GtkTreeIter *iter, gpointer user_data) {
  AppState *app = (AppState *)user_data;
  gchar *plain = NULL;
  gtk_tree_model_get(model, iter, 0, &plain, -1);
  file_view_set_search_highlight_cell(cell, app, plain);
  g_free(plain);

  /* Deleted-state styling: look up the node path. */
  const char *path = NULL;
  if (app != NULL && app->deleted_path_set != NULL && app->scan != NULL) {
    gint64 lp = 0;
    gtk_tree_model_get(model, iter, DA_COL_LP, &lp, -1);
    size_t nid = SIZE_MAX;
    if (da_tree_lp_to_scan_nid(app, lp, &nid) && nid != SIZE_MAX) {
      scan_results_view_t v = scan_get_results(app->scan);
      if (v.nodes != NULL && nid < v.count) {
        path = v.nodes[nid].path;
      }
    }
  }
  da_apply_deleted_cell_style(cell, app, path);
  da_tree_view_apply_zebra_cell(column, cell, model, iter, app, FALSE);
  g_object_set(cell, "editable", app != NULL && app->general_enable_rename, NULL);
}

static void file_view_path_text_cell_data(GtkTreeViewColumn *column, GtkCellRenderer *cell,
                                          GtkTreeModel *model, GtkTreeIter *iter, gpointer user_data) {
  AppState *app = (AppState *)user_data;
  gchar *plain = NULL;
  gtk_tree_model_get(model, iter, 1, &plain, -1);
  file_view_set_search_highlight_cell(cell, app, plain);

  /* Deleted-state styling. */
  const char *path = plain; /* col 1 IS the path */
  da_apply_deleted_cell_style(cell, app, path);
  g_free(plain);
  da_tree_view_apply_zebra_cell(column, cell, model, iter, app, FALSE);
}

static void on_file_view_name_edited(GtkCellRendererText *renderer, gchar *path, gchar *new_text, gpointer user_data) {
  (void)renderer;
  scan_controller_on_tree_name_cell_edited((AppState *)user_data, FALSE, path, new_text);
}

static void on_tree_view_name_edited(GtkCellRendererText *renderer, gchar *path, gchar *new_text, gpointer user_data) {
  (void)renderer;
  scan_controller_on_tree_name_cell_edited((AppState *)user_data, TRUE, path, new_text);
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
  gtk_tree_view_column_set_cell_data_func(c, txt, file_view_file_name_text_cell_data, app, NULL);
  g_signal_connect(txt, "edited", G_CALLBACK(on_file_view_name_edited), app);

  gtk_tree_view_column_set_alignment(c, 0.0f);
  gtk_tree_view_column_set_resizable(c, TRUE);
  gtk_tree_view_column_set_sizing(c, GTK_TREE_VIEW_COLUMN_FIXED);
  gtk_tree_view_column_set_min_width(c, min_width_px);
  gtk_tree_view_column_set_fixed_width(c, width_px);
  gtk_tree_view_column_set_sort_column_id(c, sort_model_id);
  gtk_tree_view_append_column(tv, c);
}

static void append_path_column(GtkTreeView *tv, AppState *app, const char *title, int sort_model_id, int width_px,
                               int min_width_px) {
  GtkCellRenderer *r = gtk_cell_renderer_text_new();
  g_object_set(r, "ellipsize", PANGO_ELLIPSIZE_END, "xalign", 0.0f, NULL);
  GtkTreeViewColumn *c = gtk_tree_view_column_new();
  gtk_tree_view_column_set_title(c, title);
  gtk_tree_view_column_pack_start(c, r, TRUE);
  gtk_tree_view_column_set_cell_data_func(c, r, file_view_path_text_cell_data, app, NULL);
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
  if (app->search == NULL) {
    return;
  }
  if (GTK_IS_COMBO_BOX(app->search)) {
    GtkWidget *ch = gtk_bin_get_child(GTK_BIN(app->search));
    if (ch != NULL && GTK_IS_ENTRY(ch)) {
      gtk_entry_set_text(GTK_ENTRY(ch), "");
    }
  } else if (GTK_IS_ENTRY(app->search)) {
    gtk_entry_set_text(GTK_ENTRY(app->search), "");
  }
}

static void on_file_menu_copy_activate(GtkMenuItem *item, gpointer user_data) {
  (void)item;
  AppState *app = (AppState *)user_data;
  if (app == NULL || app->window == NULL) {
    return;
  }
  scan_controller_copy_files(app);
}

static void on_file_menu_cut_activate(GtkMenuItem *item, gpointer user_data) {
  (void)item;
  AppState *app = (AppState *)user_data;
  if (app == NULL || app->window == NULL) {
    return;
  }
  scan_controller_cut_files(app);
}

static void on_file_menu_delete_trash_activate(GtkMenuItem *item, gpointer user_data) {
  (void)item;
  AppState *app = (AppState *)user_data;
  if (app == NULL || app->window == NULL) {
    return;
  }
  scan_controller_delete_to_trash(app);
}

static void on_file_menu_delete_permanent_activate(GtkMenuItem *item, gpointer user_data) {
  (void)item;
  AppState *app = (AppState *)user_data;
  if (app == NULL || app->window == NULL) {
    return;
  }
  scan_controller_delete_permanent(app);
}

static void on_file_menu_rename_activate(GtkMenuItem *item, gpointer user_data) {
  (void)item;
  AppState *app = (AppState *)user_data;
  if (app == NULL || app->window == NULL) {
    return;
  }
  scan_controller_begin_rename_selection(app);
}

static void on_file_menu_scan_activate(GtkMenuItem *item, gpointer user_data) {
  (void)item;
  scan_controller_request_scan((AppState *)user_data);
}

static void on_file_menu_select_folder_activate(GtkMenuItem *item, gpointer user_data) {
  (void)item;
  da_scan_source_combo_request_select_folder((AppState *)user_data);
}

static void on_file_menu_export_csv_activate(GtkMenuItem *item, gpointer user_data) {
  (void)item;
  AppState *app = (AppState *)user_data;
  if (app == NULL || app->window == NULL) {
    return;
  }
  if (app->scan == NULL) {
    GtkWidget *d = gtk_message_dialog_new(GTK_WINDOW(app->window), GTK_DIALOG_MODAL, GTK_MESSAGE_INFO,
                                          GTK_BUTTONS_OK, "Nothing to export (no scan data).");
    gtk_dialog_run(GTK_DIALOG(d));
    gtk_widget_destroy(d);
    return;
  }
  scan_progress_t pr = scan_get_progress(app->scan);
  if (!pr.is_complete) {
    GtkWidget *d = gtk_message_dialog_new(GTK_WINDOW(app->window), GTK_DIALOG_MODAL, GTK_MESSAGE_INFO,
                                          GTK_BUTTONS_OK, "Wait until the scan finishes before exporting.");
    gtk_dialog_run(GTK_DIALOG(d));
    gtk_widget_destroy(d);
    return;
  }

  GtkFileChooserNative *native =
      gtk_file_chooser_native_new("Export to CSV…", GTK_WINDOW(app->window), GTK_FILE_CHOOSER_ACTION_SAVE,
                                  "_Export", "_Cancel");
  GtkFileChooser *chooser = GTK_FILE_CHOOSER(native);
  gtk_file_chooser_set_current_name(chooser, "diskatlas_export.csv");

  gint resp = gtk_native_dialog_run(GTK_NATIVE_DIALOG(native));
  if (resp == GTK_RESPONSE_ACCEPT) {
    gchar *fn = gtk_file_chooser_get_filename(chooser);
    if (fn != NULL) {
      char err[512];
      if (da_export_scan_csv(app, fn, TRUE, err, sizeof err) != 0) {
        GtkWidget *d = gtk_message_dialog_new(GTK_WINDOW(app->window), GTK_DIALOG_MODAL, GTK_MESSAGE_ERROR,
                                              GTK_BUTTONS_OK, "%s", err[0] != '\0' ? err : "Export failed.");
        gtk_dialog_run(GTK_DIALOG(d));
        gtk_widget_destroy(d);
      }
      g_free(fn);
    }
  }
  gtk_native_dialog_destroy(GTK_NATIVE_DIALOG(native));
}

static void on_file_menu_copy_clipboard_activate(GtkMenuItem *item, gpointer user_data) {
  (void)item;
  AppState *app = (AppState *)user_data;
  if (app == NULL || app->window == NULL) {
    return;
  }
  scan_controller_copy_scan_paths_sizes_to_clipboard(app);
}

static void on_file_menu_explore_folder_activate(GtkMenuItem *item, gpointer user_data) {
  (void)item;
  AppState *app = (AppState *)user_data;
  if (app == NULL || app->window == NULL) {
    return;
  }
  scan_controller_explore_selected_folders(app);
}

static void on_file_menu_terminal_activate(GtkMenuItem *item, gpointer user_data) {
  (void)item;
  AppState *app = (AppState *)user_data;
  if (app == NULL || app->window == NULL) {
    return;
  }
  scan_controller_open_terminal_here(app);
}

static void on_file_menu_copy_path_activate(GtkMenuItem *item, gpointer user_data) {
  (void)item;
  AppState *app = (AppState *)user_data;
  if (app == NULL || app->window == NULL) {
    return;
  }
  scan_controller_copy_selected_paths_to_clipboard(app);
}

static void on_file_menu_import_csv_activate(GtkMenuItem *item, gpointer user_data) {
  (void)item;
  da_scan_source_combo_run_csv_import((AppState *)user_data);
}

static void on_file_menu_export_mft_activate(GtkMenuItem *item, gpointer user_data) {
  (void)item;
  AppState *app = (AppState *)user_data;
  if (app == NULL || app->window == NULL) {
    return;
  }
#if defined(G_OS_WIN32)
  if (!da_win32_is_process_elevated()) {
    GtkWidget *d =
        gtk_message_dialog_new(GTK_WINDOW(app->window), GTK_DIALOG_MODAL, GTK_MESSAGE_WARNING, GTK_BUTTONS_OK,
                               "%s", "Please restart DiskAtlas as administrator to dump the MFT file.");
    gtk_dialog_run(GTK_DIALOG(d));
    gtk_widget_destroy(d);
    return;
  }
  gchar *vol_root = NULL;
  const gchar *src_path = (app->scan_root_utf8 != NULL && app->scan_root_utf8[0] != '\0') ? app->scan_root_utf8 : "";
  if (!da_win32_path_get_volume_root_utf8(src_path, &vol_root)) {
    GtkWidget *d =
        gtk_message_dialog_new(GTK_WINDOW(app->window), GTK_DIALOG_MODAL, GTK_MESSAGE_WARNING, GTK_BUTTONS_OK,
                               "%s", "Please select an NTFS formatted drive.");
    gtk_dialog_run(GTK_DIALOG(d));
    gtk_widget_destroy(d);
    return;
  }
  if (!da_win32_volume_root_is_ntfs(vol_root)) {
    g_free(vol_root);
    GtkWidget *d =
        gtk_message_dialog_new(GTK_WINDOW(app->window), GTK_DIALOG_MODAL, GTK_MESSAGE_WARNING, GTK_BUTTONS_OK,
                               "%s", "Please select an NTFS formatted drive.");
    gtk_dialog_run(GTK_DIALOG(d));
    gtk_widget_destroy(d);
    return;
  }

  GtkFileChooserNative *native =
      gtk_file_chooser_native_new("Dump MFT file…", GTK_WINDOW(app->window), GTK_FILE_CHOOSER_ACTION_SAVE,
                                  "_Save", "_Cancel");
  GtkFileChooser *chooser = GTK_FILE_CHOOSER(native);
  gtk_file_chooser_set_current_name(chooser, "MFT");
  gint resp = gtk_native_dialog_run(GTK_NATIVE_DIALOG(native));
  if (resp != GTK_RESPONSE_ACCEPT) {
    g_free(vol_root);
    gtk_native_dialog_destroy(GTK_NATIVE_DIALOG(native));
    return;
  }
  gchar *fn = gtk_file_chooser_get_filename(chooser);
  gtk_native_dialog_destroy(GTK_NATIVE_DIALOG(native));
  if (fn == NULL || fn[0] == '\0') {
    g_free(vol_root);
    g_free(fn);
    return;
  }

  gboolean need_scan = TRUE;
  if (app->scan != NULL && !app->csv_import_active) {
    scan_progress_t pr = scan_get_progress(app->scan);
    if (pr.is_complete) {
      gchar *root2 = NULL;
      if (da_win32_path_get_volume_root_utf8(app->scan_root_utf8, &root2) &&
          g_ascii_strcasecmp(root2, vol_root) == 0) {
        need_scan = FALSE;
      }
      g_free(root2);
    }
  }

  scan_controller_begin_mft_dump_flow(app, vol_root, fn, need_scan);
  g_free(vol_root);
  g_free(fn);
#else
  GtkWidget *d = gtk_message_dialog_new(GTK_WINDOW(app->window), GTK_DIALOG_MODAL, GTK_MESSAGE_INFO,
                                        GTK_BUTTONS_OK, "%s",
                                        "Dumping the NTFS MFT file is only supported on Windows.");
  gtk_dialog_run(GTK_DIALOG(d));
  gtk_widget_destroy(d);
#endif
}

typedef struct {
  GtkDialog       *dialog;
  DaSettingsMimeCtx *mime;
  AppState        *app;
  gboolean         mime_saved;
  GtkWidget       *decimal_places_spin;
  GtkWidget       *treemap_gradient_check;
  GtkWidget       *alternate_row_colors_check;
  GtkWidget       *enable_rename_check;
} DaSettingsDlgHandles;

static void da_ui_apply_alternate_row_colors(AppState *app) {
  if (app == NULL) {
    return;
  }
  GtkWidget *widgets[] = { app->tree, app->tree_view, app->file_type_tree };
  for (gsize i = 0; i < G_N_ELEMENTS(widgets); i++) {
    GtkWidget *w = widgets[i];
    if (w == NULL || !GTK_IS_TREE_VIEW(w)) {
      continue;
    }
    gtk_widget_queue_draw(w);
  }
}

static void da_ui_apply_treemap_style(AppState *app) {
  if (app == NULL) {
    return;
  }
  if (app->treemap != NULL) {
    treemap_widget_set_style(TREEMAP_WIDGET(app->treemap), &app->treemap_style);
  }
  if (app->scan != NULL) {
    size_t count = 0;
    file_node_t *nodes = scan_result_nodes_mutable(app->scan, &count);
    if (nodes != NULL && count > 0u) {
      double shadow = app->treemap_style.gradient_strength *
                      (DM_TREEMAP_DEFAULT_SHADOW_STRENGTH / DM_TREEMAP_DEFAULT_GRADIENT_STRENGTH);
      dm_file_nodes_refresh_gradient_colors(nodes, count, app->treemap_style.gradient_strength, shadow);
    }
  }
}

static void da_settings_apply_interface_tab(DaSettingsDlgHandles *h) {
  if (h == NULL || h->app == NULL) {
    return;
  }
  gint v = h->app->size_decimal_places;
  if (h->decimal_places_spin != NULL && GTK_IS_SPIN_BUTTON(h->decimal_places_spin)) {
    v = gtk_spin_button_get_value_as_int(GTK_SPIN_BUTTON(h->decimal_places_spin));
  }
  if (v < 0) {
    v = 0;
  } else if (v > 4) {
    v = 4;
  }
  h->app->size_decimal_places = v;

  if (h->treemap_gradient_check != NULL && GTK_IS_TOGGLE_BUTTON(h->treemap_gradient_check)) {
    h->app->treemap_style.enable_tile_gradients =
        gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(h->treemap_gradient_check));
  }

  if (h->alternate_row_colors_check != NULL && GTK_IS_TOGGLE_BUTTON(h->alternate_row_colors_check)) {
    h->app->interface_alternate_row_colors =
        gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(h->alternate_row_colors_check));
  }

  if (h->enable_rename_check != NULL && GTK_IS_TOGGLE_BUTTON(h->enable_rename_check)) {
    h->app->general_enable_rename = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(h->enable_rename_check));
  }

  da_ini_save_interface(h->app);
  da_ini_save_general(h->app);
  scan_controller_refresh_size_display_format(h->app);
  da_ui_apply_treemap_style(h->app);
  da_ui_apply_alternate_row_colors(h->app);
  da_ui_apply_header_panel_visibility(h->app);
  da_ui_apply_tree_view_tab_extras_visibility(h->app);
  da_ui_sync_file_menu(h->app);
  if (h->app->tree != NULL) {
    gtk_widget_queue_draw(h->app->tree);
  }
  if (h->app->tree_view != NULL) {
    gtk_widget_queue_draw(h->app->tree_view);
  }
}

static void on_settings_ok_clicked(GtkButton *btn, gpointer user_data) {
  (void)btn;
  DaSettingsDlgHandles *h = (DaSettingsDlgHandles *)user_data;
  if (h->mime != NULL && !da_settings_mime_tab_save(h->mime, GTK_WINDOW(h->dialog))) {
    return;
  }
  h->mime_saved = TRUE;
  da_settings_apply_interface_tab(h);
  gtk_dialog_response(h->dialog, GTK_RESPONSE_OK);
}

static void on_settings_apply_clicked(GtkButton *btn, gpointer user_data) {
  (void)btn;
  DaSettingsDlgHandles *h = (DaSettingsDlgHandles *)user_data;
  if (h->mime != NULL && !da_settings_mime_tab_save(h->mime, GTK_WINDOW(h->dialog))) {
    return;
  }
  h->mime_saved = TRUE;
  da_settings_apply_interface_tab(h);
  gtk_dialog_response(h->dialog, GTK_RESPONSE_APPLY);
}

static void on_settings_cancel_clicked(GtkButton *btn, gpointer user_data) {
  (void)btn;
  DaSettingsDlgHandles *h = (DaSettingsDlgHandles *)user_data;
  gtk_dialog_response(h->dialog, GTK_RESPONSE_CANCEL);
}

/** Rebuild the runtime MIME database from the saved INI and reclassify all scan nodes. */
static void da_ui_rebuild_mime_db_and_reclassify(AppState *app) {
  dm_mime_db_free(app->mime_db);
  GPtrArray *cats = da_ini_mime_categories_load();
  app->mime_db = dm_mime_db_build(cats);
  g_ptr_array_unref(cats);
  scan_controller_reclassify_mime(app);
}

static void on_tools_menu_settings_activate(GtkMenuItem *item, gpointer user_data) {
  (void)item;
  AppState *app = (AppState *)user_data;
  if (app == NULL || app->window == NULL) {
    return;
  }

  GError *err = NULL;
  GtkBuilder *builder = gtk_builder_new();
  if (!gtk_builder_add_from_resource(builder, DISKATLAS_SETTINGS_DIALOG_RESOURCE, &err)) {
    g_warning("Failed to load settings UI (%s): %s", DISKATLAS_SETTINGS_DIALOG_RESOURCE, err->message);
    g_clear_error(&err);
    g_object_unref(builder);
    return;
  }

  GObject *obj = gtk_builder_get_object(builder, "settings_dialog");
  if (obj == NULL || !GTK_IS_DIALOG(obj)) {
    g_warning("settings_dialog object missing or not a GtkDialog");
    g_object_unref(builder);
    return;
  }

  GtkDialog *dialog = GTK_DIALOG(obj);
  gtk_window_set_transient_for(GTK_WINDOW(dialog), GTK_WINDOW(app->window));

  DaSettingsDlgHandles *handles = g_new0(DaSettingsDlgHandles, 1);
  handles->dialog = dialog;
  handles->mime = da_settings_mime_tab_bind(builder);
  handles->app = app;
  handles->mime_saved = FALSE;
  handles->decimal_places_spin = GTK_WIDGET(gtk_builder_get_object(builder, "decimal_places_spin"));
  if (handles->decimal_places_spin != NULL && GTK_IS_SPIN_BUTTON(handles->decimal_places_spin)) {
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(handles->decimal_places_spin), (gdouble)app->size_decimal_places);
  }
  handles->treemap_gradient_check = GTK_WIDGET(gtk_builder_get_object(builder, "treemap_gradient_check"));
  if (handles->treemap_gradient_check != NULL && GTK_IS_TOGGLE_BUTTON(handles->treemap_gradient_check)) {
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(handles->treemap_gradient_check),
                                 app->treemap_style.enable_tile_gradients);
  }
  handles->alternate_row_colors_check = GTK_WIDGET(gtk_builder_get_object(builder, "alternate_row_colors_check"));
  if (handles->alternate_row_colors_check != NULL && GTK_IS_TOGGLE_BUTTON(handles->alternate_row_colors_check)) {
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(handles->alternate_row_colors_check),
                                 app->interface_alternate_row_colors);
  }
  handles->enable_rename_check = GTK_WIDGET(gtk_builder_get_object(builder, "enable_rename_check"));
  if (handles->enable_rename_check != NULL && GTK_IS_TOGGLE_BUTTON(handles->enable_rename_check)) {
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(handles->enable_rename_check), app->general_enable_rename);
  }

  GtkWidget *ok_btn = GTK_WIDGET(gtk_builder_get_object(builder, "ok_btn"));
  GtkWidget *apply_btn = GTK_WIDGET(gtk_builder_get_object(builder, "apply_btn"));
  GtkWidget *cancel_btn = GTK_WIDGET(gtk_builder_get_object(builder, "cancel_btn"));
  if (ok_btn != NULL) {
    g_signal_connect(ok_btn, "clicked", G_CALLBACK(on_settings_ok_clicked), handles);
  }
  if (apply_btn != NULL) {
    g_signal_connect(apply_btn, "clicked", G_CALLBACK(on_settings_apply_clicked), handles);
  }
  if (cancel_btn != NULL) {
    g_signal_connect(cancel_btn, "clicked", G_CALLBACK(on_settings_cancel_clicked), handles);
  }

  for (;;) {
    gint resp = gtk_dialog_run(dialog);
    if (resp == GTK_RESPONSE_APPLY) {
      if (handles->mime_saved) {
        da_ui_rebuild_mime_db_and_reclassify(app);
        handles->mime_saved = FALSE;
      }
      gtk_widget_show(GTK_WIDGET(dialog));
      continue;
    }
    break;
  }

  if (handles->mime_saved) {
    da_ui_rebuild_mime_db_and_reclassify(app);
  }

  gtk_widget_destroy(GTK_WIDGET(dialog));
  da_settings_mime_tab_free(handles->mime);
  g_free(handles);
  g_object_unref(builder);
}

static void pct_of_drive_cell_data(GtkTreeViewColumn *column, GtkCellRenderer *cell, GtkTreeModel *model,
                                   GtkTreeIter *iter, gpointer user_data) {
  AppState *app = (AppState *)user_data;
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

  /* Deleted styling: resolve nid → path → apply strikethrough. */
  const char *path = NULL;
  if (app != NULL && app->deleted_path_set != NULL && app->scan != NULL) {
    gint64 lp = 0;
    gtk_tree_model_get(model, iter, DA_COL_LP, &lp, -1);
    size_t nid = SIZE_MAX;
    if (da_tree_lp_to_scan_nid(app, lp, &nid) && nid != SIZE_MAX) {
      scan_results_view_t sv = scan_get_results(app->scan);
      if (sv.nodes != NULL && nid < sv.count) {
        path = sv.nodes[nid].path;
      }
    }
  }
  da_apply_deleted_cell_style(cell, app, path);
  da_tree_view_apply_zebra_cell(column, cell, model, iter, app, FALSE);
}

static void append_pct_of_drive_column(GtkTreeView *tv, const char *title, int sort_model_id, int width_px,
                                       int min_width_px, AppState *app) {
  GtkCellRenderer *r = da_cell_renderer_progress_new();
  g_object_set(r, "xpad", 0, "ypad", 0, "xalign", 0.0f, NULL);
  GtkTreeViewColumn *c = gtk_tree_view_column_new();
  gtk_tree_view_column_set_title(c, title);
  gtk_tree_view_column_pack_start(c, r, TRUE);
  gtk_tree_view_column_set_cell_data_func(c, r, pct_of_drive_cell_data, app, NULL);
  gtk_tree_view_column_set_alignment(c, 0.0f);
  gtk_tree_view_column_set_resizable(c, TRUE);
  gtk_tree_view_column_set_sizing(c, GTK_TREE_VIEW_COLUMN_FIXED);
  gtk_tree_view_column_set_min_width(c, min_width_px);
  gtk_tree_view_column_set_fixed_width(c, width_px);
  gtk_tree_view_column_set_sort_column_id(c, sort_model_id);
  gtk_tree_view_append_column(tv, c);
}

typedef struct {
  AppState *app;
  gint      model_col;
  gboolean  is_alloc_col;
} DaSimpleTextColCtx;

static void da_simple_text_cell_data(GtkTreeViewColumn *col, GtkCellRenderer *cell, GtkTreeModel *model,
                                     GtkTreeIter *iter, gpointer user_data) {
  DaSimpleTextColCtx *ctx = (DaSimpleTextColCtx *)user_data;
  if (ctx == NULL) {
    return;
  }
  gchar *text = NULL;
  gtk_tree_model_get(model, iter, ctx->model_col, &text, -1);
  g_object_set(cell, "text", text, NULL);
  g_free(text);
  da_tree_view_apply_zebra_cell(col, cell, model, iter, ctx->app, ctx->is_alloc_col);
}

static void append_text_column(GtkTreeView *tv, AppState *app, const char *title, int model_col, int sort_model_id,
                               int width_px, int min_width_px, gfloat xalign, gboolean is_alloc_column) {
  GtkCellRenderer *r = gtk_cell_renderer_text_new();
  g_object_set(r, "ellipsize", PANGO_ELLIPSIZE_END, "xalign", xalign, NULL);
  GtkTreeViewColumn *c = gtk_tree_view_column_new();
  gtk_tree_view_column_set_title(c, title);
  gtk_tree_view_column_pack_start(c, r, TRUE);
  DaSimpleTextColCtx *ctx = g_new(DaSimpleTextColCtx, 1);
  ctx->app          = app;
  ctx->model_col    = model_col;
  ctx->is_alloc_col = is_alloc_column;
  gtk_tree_view_column_set_cell_data_func(c, r, da_simple_text_cell_data, ctx, g_free);
  gtk_tree_view_column_set_alignment(c, 0.0f);
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
  AppState *app = (AppState *)user_data;
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
  da_tree_view_apply_zebra_cell(column, cell, model, iter, app, FALSE);
}

static void tv_pct_of_parent_cell_data(GtkTreeViewColumn *column, GtkCellRenderer *cell,
                                       GtkTreeModel *model, GtkTreeIter *iter,
                                       gpointer user_data) {
  AppState *app = (AppState *)user_data;
  gint pv = -1;
  gchar *txt = NULL;
  gchar *path = NULL;
  gtk_tree_model_get(model, iter, DA_TV_COL_PCT_VAL, &pv, DA_TV_COL_PCT_LABEL, &txt,
                     DA_TV_COL_PATH, &path, -1);
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

  /* Deleted styling: read path from model → apply strikethrough. */
  da_apply_deleted_cell_style(cell, app, path);
  g_free(path);
  da_tree_view_apply_zebra_cell(column, cell, model, iter, app, FALSE);
}

static void append_tv_pct_column(GtkTreeView *tv, const char *title, int width_px, int min_width_px,
                                 AppState *app) {
  GtkCellRenderer *r = da_cell_renderer_progress_new();
  g_object_set(r, "xpad", 0, "ypad", 0, "xalign", 0.0f, NULL);
  GtkTreeViewColumn *c = gtk_tree_view_column_new();
  gtk_tree_view_column_set_title(c, title);
  gtk_tree_view_column_pack_start(c, r, TRUE);
  gtk_tree_view_column_set_cell_data_func(c, r, tv_pct_of_parent_cell_data, app, NULL);
  gtk_tree_view_column_set_alignment(c, 0.0f);
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
    gtk_tree_view_column_set_cell_data_func(c, pix, tv_icon_cell_data, app, NULL);
    gtk_tree_view_column_pack_start(c, txt, TRUE);
    /* Use a data function (instead of attribute binding) so deleted-state styling can be applied. */
    {
      DaDeletedCellCtx *ctx = g_new(DaDeletedCellCtx, 1);
      ctx->app            = app;
      ctx->model_text_col = DA_TV_COL_NAME;
      ctx->is_tree_view   = TRUE;
      gtk_tree_view_column_set_cell_data_func(c, txt, da_tv_deleted_text_cell_data, ctx, g_free);
    }
    g_signal_connect(txt, "edited", G_CALLBACK(on_tree_view_name_edited), app);
    gtk_tree_view_column_set_alignment(c, 0.0f);
    gtk_tree_view_column_set_resizable(c, TRUE);
    gtk_tree_view_column_set_sizing(c, GTK_TREE_VIEW_COLUMN_FIXED);
    gtk_tree_view_column_set_min_width(c, 140);
    gtk_tree_view_column_set_fixed_width(c, 300);
    gtk_tree_view_column_set_sort_column_id(c, DA_TV_COL_NAME);
    gtk_tree_view_append_column(GTK_TREE_VIEW(app->tree_view), c);
  }

  /* Column 1: Percent of Parent (progress bar). */
  append_tv_pct_column(GTK_TREE_VIEW(app->tree_view), "Percent of Parent", 130, 88, app);

  /* Remaining text columns: Size, Allocated, Items, Files, Folders, Modified, Attributes. */
  static const char  *tv_titles[] = { "Size", "Allocated", "Items", "Files", "Folders", "Modified", "Attributes" };
  static const int    tv_cols[]   = { DA_TV_COL_SIZE, DA_TV_COL_ALLOC, DA_TV_COL_ITEMS,
                                      DA_TV_COL_FILES, DA_TV_COL_FOLDERS, DA_TV_COL_MODIFIED, DA_TV_COL_ATTRS };
  static const int    tv_widths[] = { 100, 100, 80, 80, 80, 140, 68 };
  static const int    tv_minw[]   = { 72, 72, 56, 56, 56, 100, 48 };
  static const gfloat tv_align[]  = { 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 0.0f, 0.0f };
  for (int i = 0; i < 7; i++) {
    gboolean is_alloc = (tv_cols[i] == DA_TV_COL_ALLOC);
    append_text_column(GTK_TREE_VIEW(app->tree_view), app, tv_titles[i], tv_cols[i], tv_cols[i],
                       tv_widths[i], tv_minw[i], tv_align[i], is_alloc);
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
  da_file_view_search_combo_init(app);
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
  app->file_type_tree = GTK_WIDGET(gtk_builder_get_object(builder, "file_type_tree"));
  if (app->file_type_tree != NULL) {
    gtk_style_context_add_class(gtk_widget_get_style_context(app->file_type_tree),
                                DISKATLAS_TREE_PROGRESS_STYLE_CLASS);
    da_file_type_view_setup(app);
  }
  app->file_type_scrolled = GTK_WIDGET(gtk_builder_get_object(builder, "file_type_scrolled"));
  app->treemap_panel = GTK_WIDGET(gtk_builder_get_object(builder, "treemap_panel"));
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
  /* Keeps status bar + treemap header height stable when paths ellipsize (hover updates center). */
  gtk_label_set_single_line_mode(GTK_LABEL(app->treemap_panel_title), TRUE);
  gtk_label_set_single_line_mode(GTK_LABEL(app->status_label_left), TRUE);
  gtk_label_set_single_line_mode(GTK_LABEL(app->status_label_center), TRUE);
  gtk_label_set_single_line_mode(GTK_LABEL(app->status_label_right), TRUE);

  app->stat_sel_val = GTK_WIDGET(gtk_builder_get_object(builder, "stat_sel_val"));
  app->stat_tot_val = GTK_WIDGET(gtk_builder_get_object(builder, "stat_tot_val"));
  app->stat_use_val = GTK_WIDGET(gtk_builder_get_object(builder, "stat_use_val"));
  app->stat_free_val = GTK_WIDGET(gtk_builder_get_object(builder, "stat_free_val"));
  app->header_panel = GTK_WIDGET(gtk_builder_get_object(builder, "header_panel"));

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

  da_ini_load_filetree(app);
  da_ini_load_interface(app);
  da_ini_load_general(app);
  da_ui_apply_header_panel_visibility(app);
  da_ui_apply_tree_view_tab_extras_visibility(app);

  treemap_widget_set_style(TREEMAP_WIDGET(app->treemap), &app->treemap_style);

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
    if (i == 1) {
      append_path_column(GTK_TREE_VIEW(app->tree), app, titles[i], col_sort_id[i], col_w[i], col_min_w[i]);
      continue;
    }
    if (i == 2) {
      append_pct_of_drive_column(GTK_TREE_VIEW(app->tree), titles[i], col_sort_id[i], col_w[i], col_min_w[i], app);
      continue;
    }
    gfloat xalign = 0.0f;
    if (i == 3 || i == 4 || i == 6 || i == 7) {
      xalign = 1.0f;
    }
    gboolean is_alloc = (i == DA_COL_ALLOCATED);
    append_text_column(GTK_TREE_VIEW(app->tree), app, titles[i], i, col_sort_id[i], col_w[i], col_min_w[i], xalign,
                       is_alloc);
  }

  /* Sorting is handled internally by FlatListModel via GtkTreeSortable.
   * fixed_height_mode is safe with children as long as all rows (parent and
   * child) use the same cell renderers at the same font size — which they do. */
  gtk_tree_view_set_fixed_height_mode(GTK_TREE_VIEW(app->tree), TRUE);

  /* Install deleted-state cell data functions on file view text columns 3–7
   * (Size=3, Allocated=4, Modified=5, Dup Count=6, Dup Size=7).
   * Columns 0 (File Name) and 1 (Path) already have data functions modified above.
   * Column 2 (% of used space) is a progress bar — skip.
   * Column 8 (Attributes) is excluded per spec. */
  {
    static const gint fv_cols[] = { 3, 4, 5, 6, 7 };
    for (gint fi = 0; fi < (gint)G_N_ELEMENTS(fv_cols); fi++) {
      da_install_deleted_text_style(GTK_TREE_VIEW(app->tree), fv_cols[fi], app,
                                    fv_cols[fi], FALSE);
    }
  }

  da_setup_tree_view(app);

  /* Install deleted-state cell data functions on tree view text columns 2–7
   * (view indices): Size(2), Allocated(3), Items(4), Files(5), Folders(6), Modified(7).
   * Column 0 (Folder) is handled directly in da_setup_tree_view.
   * Column 1 (Percent of Parent) is a progress bar — skip.
   * Column 8 (Attributes) is excluded per spec.
   * The model columns match tv_cols[] in da_setup_tree_view. */
  {
    static const gint tv_view_idx[] = { 2, 3, 4, 5, 6, 7 };
    static const gint tv_mdl_col[]  = {
      DA_TV_COL_SIZE, DA_TV_COL_ALLOC, DA_TV_COL_ITEMS,
      DA_TV_COL_FILES, DA_TV_COL_FOLDERS, DA_TV_COL_MODIFIED
    };
    for (gint ti = 0; ti < (gint)G_N_ELEMENTS(tv_view_idx); ti++) {
      da_install_deleted_text_style(GTK_TREE_VIEW(app->tree_view), tv_view_idx[ti], app,
                                    tv_mdl_col[ti], TRUE);
    }
  }

  da_ui_apply_alternate_row_colors(app);
  da_ui_sync_file_menu(app);
  if (app->tree != NULL) {
    gtk_widget_queue_draw(app->tree);
  }
  if (app->tree_view != NULL) {
    gtk_widget_queue_draw(app->tree_view);
  }

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

  {
    GtkWidget *file_menu_scan = GTK_WIDGET(gtk_builder_get_object(builder, "file_menu_scan"));
    if (file_menu_scan != NULL) {
      g_signal_connect(file_menu_scan, "activate", G_CALLBACK(on_file_menu_scan_activate), app);
    }
  }
  {
    GtkWidget *file_menu_select_folder = GTK_WIDGET(gtk_builder_get_object(builder, "file_menu_select_folder"));
    if (file_menu_select_folder != NULL) {
      g_signal_connect(file_menu_select_folder, "activate", G_CALLBACK(on_file_menu_select_folder_activate), app);
    }
  }
  {
    GtkWidget *file_menu_export_csv = GTK_WIDGET(gtk_builder_get_object(builder, "file_menu_export_csv"));
    app->file_menu_export_csv = file_menu_export_csv;
    if (file_menu_export_csv != NULL) {
      g_signal_connect(file_menu_export_csv, "activate", G_CALLBACK(on_file_menu_export_csv_activate), app);
    }
  }
  {
    GtkWidget *file_menu_explore_folder = GTK_WIDGET(gtk_builder_get_object(builder, "file_menu_explore_folder"));
    app->file_menu_explore_folder = file_menu_explore_folder;
    if (file_menu_explore_folder != NULL) {
      g_signal_connect(file_menu_explore_folder, "activate", G_CALLBACK(on_file_menu_explore_folder_activate), app);
    }
  }
  {
    GtkWidget *file_menu_terminal = GTK_WIDGET(gtk_builder_get_object(builder, "file_menu_terminal"));
    app->file_menu_terminal = file_menu_terminal;
    if (file_menu_terminal != NULL && GTK_IS_MENU_ITEM(file_menu_terminal)) {
#if defined(G_OS_WIN32)
      gtk_menu_item_set_label(GTK_MENU_ITEM(file_menu_terminal), "Command Prompt Here");
#else
      gtk_menu_item_set_label(GTK_MENU_ITEM(file_menu_terminal), "Terminal Here");
#endif
      g_signal_connect(file_menu_terminal, "activate", G_CALLBACK(on_file_menu_terminal_activate), app);
    }
  }
  {
    GtkWidget *file_menu_copy_path = GTK_WIDGET(gtk_builder_get_object(builder, "file_menu_copy_path"));
    app->file_menu_copy_path = file_menu_copy_path;
    if (file_menu_copy_path != NULL) {
      g_signal_connect(file_menu_copy_path, "activate", G_CALLBACK(on_file_menu_copy_path_activate), app);
    }
  }
  {
    GtkWidget *w = GTK_WIDGET(gtk_builder_get_object(builder, "file_menu_copy"));
    app->file_menu_copy = w;
    if (w != NULL) {
      g_signal_connect(w, "activate", G_CALLBACK(on_file_menu_copy_activate), app);
    }
  }
  {
    GtkWidget *w = GTK_WIDGET(gtk_builder_get_object(builder, "file_menu_cut"));
    app->file_menu_cut = w;
    if (w != NULL) {
      g_signal_connect(w, "activate", G_CALLBACK(on_file_menu_cut_activate), app);
    }
  }
  {
    GtkWidget *w = GTK_WIDGET(gtk_builder_get_object(builder, "file_menu_delete_trash"));
    app->file_menu_delete_trash = w;
    if (w != NULL && GTK_IS_MENU_ITEM(w)) {
#if !defined(G_OS_WIN32)
      gtk_menu_item_set_label(GTK_MENU_ITEM(w), "Delete (to Trash)");
#endif
      g_signal_connect(w, "activate", G_CALLBACK(on_file_menu_delete_trash_activate), app);
    }
  }
  {
    GtkWidget *w = GTK_WIDGET(gtk_builder_get_object(builder, "file_menu_delete_permanent"));
    app->file_menu_delete_permanent = w;
    if (w != NULL) {
      g_signal_connect(w, "activate", G_CALLBACK(on_file_menu_delete_permanent_activate), app);
    }
  }
  {
    GtkWidget *w = GTK_WIDGET(gtk_builder_get_object(builder, "file_menu_rename"));
    app->file_menu_rename = w;
    if (w != NULL) {
      g_signal_connect(w, "activate", G_CALLBACK(on_file_menu_rename_activate), app);
    }
  }
  {
    GtkWidget *file_menu_copy_clipboard = GTK_WIDGET(gtk_builder_get_object(builder, "file_menu_copy_clipboard"));
    app->file_menu_copy_clipboard = file_menu_copy_clipboard;
    if (file_menu_copy_clipboard != NULL) {
      g_signal_connect(file_menu_copy_clipboard, "activate", G_CALLBACK(on_file_menu_copy_clipboard_activate), app);
    }
  }
  {
    GtkWidget *file_menu_import_csv = GTK_WIDGET(gtk_builder_get_object(builder, "file_menu_import_csv"));
    if (file_menu_import_csv != NULL) {
      g_signal_connect(file_menu_import_csv, "activate", G_CALLBACK(on_file_menu_import_csv_activate), app);
    }
  }
  {
    GtkWidget *file_menu_export_mft = GTK_WIDGET(gtk_builder_get_object(builder, "file_menu_export_mft"));
    if (file_menu_export_mft != NULL) {
      g_signal_connect(file_menu_export_mft, "activate", G_CALLBACK(on_file_menu_export_mft_activate), app);
    }
  }
  {
    GtkWidget *options_menu_settings = GTK_WIDGET(gtk_builder_get_object(builder, "options_menu_settings"));
    if (options_menu_settings != NULL) {
      g_signal_connect(options_menu_settings, "activate", G_CALLBACK(on_tools_menu_settings_activate), app);
    }
  }
  {
    GtkWidget *show_header_mi = GTK_WIDGET(gtk_builder_get_object(builder, "options_menu_show_header"));
    if (show_header_mi != NULL && GTK_IS_CHECK_MENU_ITEM(show_header_mi)) {
      g_signal_handlers_block_by_func(show_header_mi, G_CALLBACK(on_options_menu_show_header_toggled), app);
      gtk_check_menu_item_set_active(GTK_CHECK_MENU_ITEM(show_header_mi), app->interface_show_header);
      g_signal_handlers_unblock_by_func(show_header_mi, G_CALLBACK(on_options_menu_show_header_toggled), app);
      g_signal_connect(show_header_mi, "toggled", G_CALLBACK(on_options_menu_show_header_toggled), app);
    }
  }
  {
    GtkWidget *show_ft_mi = GTK_WIDGET(gtk_builder_get_object(builder, "options_menu_show_file_types"));
    if (show_ft_mi != NULL && GTK_IS_CHECK_MENU_ITEM(show_ft_mi)) {
      g_signal_handlers_block_by_func(show_ft_mi, G_CALLBACK(on_options_menu_show_file_types_toggled), app);
      gtk_check_menu_item_set_active(GTK_CHECK_MENU_ITEM(show_ft_mi), app->interface_show_file_types);
      g_signal_handlers_unblock_by_func(show_ft_mi, G_CALLBACK(on_options_menu_show_file_types_toggled), app);
      g_signal_connect(show_ft_mi, "toggled", G_CALLBACK(on_options_menu_show_file_types_toggled), app);
    }
  }
  {
    GtkWidget *show_tm_mi = GTK_WIDGET(gtk_builder_get_object(builder, "options_menu_show_treemap"));
    if (show_tm_mi != NULL && GTK_IS_CHECK_MENU_ITEM(show_tm_mi)) {
      g_signal_handlers_block_by_func(show_tm_mi, G_CALLBACK(on_options_menu_show_treemap_toggled), app);
      gtk_check_menu_item_set_active(GTK_CHECK_MENU_ITEM(show_tm_mi), app->interface_show_treemap);
      g_signal_handlers_unblock_by_func(show_tm_mi, G_CALLBACK(on_options_menu_show_treemap_toggled), app);
      g_signal_connect(show_tm_mi, "toggled", G_CALLBACK(on_options_menu_show_treemap_toggled), app);
    }
  }
  {
    GtkWidget *w = GTK_WIDGET(gtk_builder_get_object(builder, "options_menu_show_free_space"));
    if (w != NULL && GTK_IS_CHECK_MENU_ITEM(w)) {
      g_signal_handlers_block_by_func(w, G_CALLBACK(on_options_menu_show_free_space_toggled), app);
      gtk_check_menu_item_set_active(GTK_CHECK_MENU_ITEM(w), app->interface_treemap_show_free_space);
      g_signal_handlers_unblock_by_func(w, G_CALLBACK(on_options_menu_show_free_space_toggled), app);
      g_signal_connect(w, "toggled", G_CALLBACK(on_options_menu_show_free_space_toggled), app);
    }
  }
  {
    GtkWidget *w = GTK_WIDGET(gtk_builder_get_object(builder, "options_menu_show_free_labels"));
    if (w != NULL && GTK_IS_CHECK_MENU_ITEM(w)) {
      g_signal_handlers_block_by_func(w, G_CALLBACK(on_options_menu_show_free_labels_toggled), app);
      gtk_check_menu_item_set_active(GTK_CHECK_MENU_ITEM(w), app->interface_treemap_show_labels);
      g_signal_handlers_unblock_by_func(w, G_CALLBACK(on_options_menu_show_free_labels_toggled), app);
      g_signal_connect(w, "toggled", G_CALLBACK(on_options_menu_show_free_labels_toggled), app);
    }
  }
  {
    GtkWidget *w = GTK_WIDGET(gtk_builder_get_object(builder, "file_menu_zoom_in"));
    app->file_menu_zoom_in = w;
    if (w != NULL) {
      g_signal_connect(w, "activate", G_CALLBACK(on_file_menu_zoom_in_activate), app);
    }
  }
  {
    GtkWidget *w = GTK_WIDGET(gtk_builder_get_object(builder, "file_menu_zoom_out"));
    app->file_menu_zoom_out = w;
    if (w != NULL) {
      g_signal_connect(w, "activate", G_CALLBACK(on_file_menu_zoom_out_activate), app);
    }
  }
  {
    GtkWidget *w = GTK_WIDGET(gtk_builder_get_object(builder, "file_menu_treemap_image"));
    if (w != NULL) {
      g_signal_connect(w, "activate", G_CALLBACK(on_file_menu_treemap_image_activate), app);
    }
  }
  {
    static const struct {
      const char *id;
      gint fmt;
    } size_display_items[] = {
      { "size_display_dynamic_menu", DA_SIZE_DISPLAY_DYNAMIC },
      { "size_display_byte_menu", DA_SIZE_DISPLAY_BYTES },
      { "size_display_kb_menu", DA_SIZE_DISPLAY_KB },
      { "size_display_mb_menu", DA_SIZE_DISPLAY_MB },
      { "size_display_gb_menu", DA_SIZE_DISPLAY_GB },
      { "size_display_tb_menu", DA_SIZE_DISPLAY_TB },
    };
    for (gsize si = 0; si < G_N_ELEMENTS(size_display_items); si++) {
      GtkWidget *w = GTK_WIDGET(gtk_builder_get_object(builder, size_display_items[si].id));
      if (w == NULL || !GTK_IS_CHECK_MENU_ITEM(w)) {
        continue;
      }
      g_object_set_data(G_OBJECT(w), DA_SIZE_FMT_MENU_DATA_KEY, GINT_TO_POINTER(size_display_items[si].fmt));
      g_signal_connect(w, "activate", G_CALLBACK(on_size_display_format_activate), app);
      if (size_display_items[si].fmt == app->interface_size_display_format) {
        g_signal_handlers_block_by_func(w, G_CALLBACK(on_size_display_format_activate), app);
        gtk_check_menu_item_set_active(GTK_CHECK_MENU_ITEM(w), TRUE);
        g_signal_handlers_unblock_by_func(w, G_CALLBACK(on_size_display_format_activate), app);
      }
    }
  }

  /* ---- context menu -------------------------------------------------------- */
  {
    GtkWidget *cm = GTK_WIDGET(gtk_builder_get_object(builder, "context_menu"));
    app->context_menu = cm;
    if (cm != NULL) {
      /* Keep the standalone menu alive after the builder is unreffed. */
      g_object_ref_sink(cm);

      app->context_menu_explore_folder =
          GTK_WIDGET(gtk_builder_get_object(builder, "context_menu_explore_folder"));
      app->context_menu_terminal_here =
          GTK_WIDGET(gtk_builder_get_object(builder, "context_menu_terminal_here"));
      app->context_menu_copy_path =
          GTK_WIDGET(gtk_builder_get_object(builder, "context_menu_copy_path"));
      app->context_menu_export_csv =
          GTK_WIDGET(gtk_builder_get_object(builder, "context_menu_export_csv"));
      app->context_menu_copy_file_info =
          GTK_WIDGET(gtk_builder_get_object(builder, "context_menu_copy_file_info"));
      app->context_menu_zoom_in =
          GTK_WIDGET(gtk_builder_get_object(builder, "context_menu_zoom_in"));
      app->context_menu_zoom_out =
          GTK_WIDGET(gtk_builder_get_object(builder, "context_menu_zoom_out"));

      if (app->context_menu_terminal_here != NULL && GTK_IS_MENU_ITEM(app->context_menu_terminal_here)) {
#if defined(G_OS_WIN32)
        gtk_menu_item_set_label(GTK_MENU_ITEM(app->context_menu_terminal_here), "Command Prompt Here");
#else
        gtk_menu_item_set_label(GTK_MENU_ITEM(app->context_menu_terminal_here), "Terminal Here");
#endif
      }

      if (app->context_menu_explore_folder != NULL) {
        g_signal_connect(app->context_menu_explore_folder, "activate",
                         G_CALLBACK(on_file_menu_explore_folder_activate), app);
      }
      if (app->context_menu_terminal_here != NULL) {
        g_signal_connect(app->context_menu_terminal_here, "activate",
                         G_CALLBACK(on_file_menu_terminal_activate), app);
      }
      if (app->context_menu_copy_path != NULL) {
        g_signal_connect(app->context_menu_copy_path, "activate",
                         G_CALLBACK(on_file_menu_copy_path_activate), app);
      }
      if (app->context_menu_export_csv != NULL) {
        g_signal_connect(app->context_menu_export_csv, "activate",
                         G_CALLBACK(on_file_menu_export_csv_activate), app);
      }
      if (app->context_menu_copy_file_info != NULL) {
        g_signal_connect(app->context_menu_copy_file_info, "activate",
                         G_CALLBACK(on_file_menu_copy_clipboard_activate), app);
      }
      if (app->context_menu_zoom_in != NULL) {
        g_signal_connect(app->context_menu_zoom_in, "activate",
                         G_CALLBACK(on_file_menu_zoom_in_activate), app);
      }
      if (app->context_menu_zoom_out != NULL) {
        g_signal_connect(app->context_menu_zoom_out, "activate",
                         G_CALLBACK(on_file_menu_zoom_out_activate), app);
      }

      /* Attach right-click handler to the three target widgets. */
      if (app->treemap != NULL) {
        gtk_widget_add_events(app->treemap, GDK_BUTTON_PRESS_MASK);
        g_signal_connect(app->treemap, "button-press-event",
                         G_CALLBACK(on_widget_right_click), app);
      }
      if (app->tree != NULL) {
        g_signal_connect(app->tree, "button-press-event",
                         G_CALLBACK(on_widget_right_click), app);
      }
      if (app->tree_view != NULL) {
        g_signal_connect(app->tree_view, "button-press-event",
                         G_CALLBACK(on_widget_right_click), app);
      }
    }
  }

  g_object_unref(builder);

  scan_controller_sync_display_max_combo(app);
  scan_controller_attach(app);
  scan_controller_refresh_volume_labels(app);
  da_ui_sync_file_menu(app);

  gtk_widget_show_all(app->window);
  /* show_all reveals all descendants — re-apply header / Tree View pane visibility from saved preferences. */
  da_ui_apply_header_panel_visibility(app);
  da_ui_apply_tree_view_tab_extras_visibility(app);

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
