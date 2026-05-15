#include "scan_source_combo.h"

#include <string.h>

#include <glib.h>
#include <gtk/gtk.h>

#include "diskatlas.h"
#include "scan_controller.h"
#include "shell_icon.h"
#include "volumes.h"
#include "da_message_dialog.h"

typedef enum {
  DA_SCAN_ROW_KIND_PATH = 0,
  DA_SCAN_ROW_KIND_BROWSE_FOLDER = 1,
  DA_SCAN_ROW_KIND_MFT_FILE = 2,
  DA_SCAN_ROW_KIND_CSV_FILE = 3,
} DaScanRowKind;

enum {
  DA_SCAN_COL_ICON = 0,
  DA_SCAN_COL_LABEL,
  DA_SCAN_COL_PATH,
  DA_SCAN_COL_KIND,
  DA_SCAN_N_COLS,
};

static gboolean g_scan_source_combo_internal;

/** GTK/Glib on Windows can emit GLib-GIO CRITICAL from IFileDialog-backed GtkFileChooserNative when internal
 *  code calls g_file_info_get_file_type() on GFileInfo objects without standard::type (GLib 2.84+).
 *  GLib allows only one g_log_set_writer_func() registration — install this wrapper once at startup; it
 *  forwards everything else to g_log_writer_default(). */
static GLogWriterOutput da_app_glib_log_writer(GLogLevelFlags level, const GLogField *fields, gsize n_fields,
                                               gpointer user_data) {
  (void)user_data;
  if ((level & G_LOG_LEVEL_CRITICAL) != 0 && fields != NULL) {
    const gchar *msg = NULL;
    for (gsize i = 0; i < n_fields; i++) {
      if (g_strcmp0(fields[i].key, "MESSAGE") == 0) {
        msg = (const gchar *)fields[i].value;
        break;
      }
    }
    if (msg != NULL &&
        (strstr(msg, "g_file_info_get_file_type") != NULL ||
         strstr(msg, "GFileInfo created without standard::type") != NULL)) {
      return G_LOG_WRITER_HANDLED;
    }
  }
  return g_log_writer_default(level, fields, n_fields, NULL);
}

void da_scan_source_install_gio_log_filter_once(void) {
  static gboolean installed;
  if (installed) {
    return;
  }
  g_log_set_writer_func(da_app_glib_log_writer, NULL, NULL);
  installed = TRUE;
}

static void append_row(GtkListStore *store, GdkPixbuf *icon_owned, const gchar *label, const gchar *path,
                       DaScanRowKind kind) {
  GtkTreeIter it;
  gtk_list_store_append(store, &it);
  gtk_list_store_set(store, &it, DA_SCAN_COL_ICON, icon_owned, DA_SCAN_COL_LABEL, label, DA_SCAN_COL_PATH,
                     path != NULL ? path : "", DA_SCAN_COL_KIND, (gint)kind, -1);
  if (icon_owned != NULL) {
    g_object_unref(icon_owned);
  }
}

/** Row index for `gtk_combo_box_set_active` / `iter_nth_child`.
 *  Without file row: [vol0..nv-1][Browse][MFT][CSV] — Browse index = nv. */
static gint pick_active_row(AppState *app, gboolean show_custom_file_row, DaVolumeEntry *vols, gsize nv) {
  if (show_custom_file_row) {
    return 0;
  }

  if (app->csv_import_active) {
    /* Snapshot import (CSV or raw $MFT dump): always the last row so GTK "changed"
     * does not target "<MFT file>" (which would reopen the browse/MFT dialog mid-refresh). */
    return (gint)(nv + 2u);
  }

  if (app->scan_root_utf8 != NULL && app->scan_root_utf8[0] != '\0') {
    for (gsize i = 0; i < nv; i++) {
      if (da_volume_is_exact_root_path(app->scan_root_utf8, vols[i].root_path)) {
        return (gint)i;
      }
    }
    /* Folder path that is not exactly a volume root (e.g. D:\Photos): keep "<Select Folder...>" selected. */
    if (g_file_test(app->scan_root_utf8, G_FILE_TEST_IS_DIR)) {
      return (gint)nv;
    }
  }

  gchar *sys = da_volume_system_root_utf8();
  if (sys != NULL && nv > 0) {
    for (gsize i = 0; i < nv; i++) {
      if (g_strcmp0(vols[i].root_path, sys) == 0) {
        g_free(sys);
        return (gint)i;
      }
    }
  }
  g_free(sys);

  if (nv > 0) {
    return 0;
  }
  return 0;
}

static void da_scan_source_combo_append_specials(GtkListStore *store) {
  append_row(store, NULL, "<Select Folder...>", "", DA_SCAN_ROW_KIND_BROWSE_FOLDER);
  append_row(store, NULL, "<MFT file>", "", DA_SCAN_ROW_KIND_MFT_FILE);
  append_row(store, NULL, "<CSV File>", "", DA_SCAN_ROW_KIND_CSV_FILE);
}

void da_scan_source_combo_rebuild(AppState *app) {
  if (app->scan_source_combo == NULL) {
    return;
  }

  GtkListStore *store =
      GTK_LIST_STORE(gtk_combo_box_get_model(GTK_COMBO_BOX(app->scan_source_combo)));
  if (store == NULL) {
    return;
  }

  DaVolumeEntry *vols = NULL;
  gsize nv = 0;
  if (da_volume_enumerate(&vols, &nv) != 0) {
    da_volume_list_free(vols, nv);
    return;
  }

  gboolean scan_root_is_file =
      (app->scan_root_utf8 != NULL && app->scan_root_utf8[0] != '\0' &&
       g_file_test(app->scan_root_utf8, G_FILE_TEST_IS_REGULAR));

  gboolean show_custom_file_row = scan_root_is_file && !app->csv_import_active;

  g_scan_source_combo_internal = TRUE;
  gtk_list_store_clear(store);

  if (show_custom_file_row) {
    GdkPixbuf *ic = da_shell_icon_for_path(app->scan_root_utf8, 16);
    char label_buf[640];
    da_volume_selection_label(app->scan_root_utf8, label_buf, sizeof(label_buf));
    const gchar *lab = label_buf[0] != '\0' ? label_buf : app->scan_root_utf8;
    append_row(store, ic, lab, app->scan_root_utf8, DA_SCAN_ROW_KIND_PATH);
  }

  for (gsize i = 0; i < nv; i++) {
    GdkPixbuf *ic = da_shell_icon_for_path(vols[i].root_path, 16);
    append_row(store, ic, vols[i].display_label, vols[i].root_path, DA_SCAN_ROW_KIND_PATH);
  }

  da_scan_source_combo_append_specials(store);

  gint active = pick_active_row(app, show_custom_file_row, vols, nv);
  GtkTreeModel *model = GTK_TREE_MODEL(store);
  GtkTreeIter it;
  g_signal_handlers_block_by_func(app->scan_source_combo, G_CALLBACK(da_scan_source_combo_on_changed), app);
  if (gtk_tree_model_iter_nth_child(model, &it, NULL, active)) {
    gtk_combo_box_set_active_iter(GTK_COMBO_BOX(app->scan_source_combo), &it);
  } else {
    gtk_combo_box_set_active(GTK_COMBO_BOX(app->scan_source_combo), 0);
    app->scan_source_last_stable_active = 0;
    g_signal_handlers_unblock_by_func(app->scan_source_combo, G_CALLBACK(da_scan_source_combo_on_changed), app);
    g_scan_source_combo_internal = FALSE;
    da_volume_list_free(vols, nv);
    return;
  }
  app->scan_source_last_stable_active = active;
  g_signal_handlers_unblock_by_func(app->scan_source_combo, G_CALLBACK(da_scan_source_combo_on_changed), app);

  g_scan_source_combo_internal = FALSE;

  da_volume_list_free(vols, nv);
}

/** GtkFileChooserNative + SELECT_FOLDER on Windows often leaves gtk_file_chooser_get_filename() at the volume
 *  root; gtk_file_chooser_get_file() matches the folder the user picked. */
static gchar *da_folder_path_from_chooser(GtkFileChooser *chooser) {
  GFile *gf = gtk_file_chooser_get_file(chooser);
  if (gf != NULL) {
    gchar *p = g_file_get_path(gf);
    g_object_unref(gf);
    if (p != NULL && p[0] != '\0') {
      return p;
    }
    g_free(p);
  }
  return gtk_file_chooser_get_filename(chooser);
}

static void run_select_folder_dialog(AppState *app) {
  /* GtkFileChooserNative uses the platform folder picker (Windows/macOS) or portal where available. */
  GtkFileChooserNative *native =
      gtk_file_chooser_native_new("Select Folder", GTK_WINDOW(app->window),
                                  GTK_FILE_CHOOSER_ACTION_SELECT_FOLDER, "_Select", "_Cancel");
  GtkFileChooser *chooser = GTK_FILE_CHOOSER(native);

  if (app->scan_root_utf8 != NULL && app->scan_root_utf8[0] != '\0') {
    GError *err = NULL;
    if (g_file_test(app->scan_root_utf8, G_FILE_TEST_IS_DIR)) {
      GFile *gf = g_file_new_for_path(app->scan_root_utf8);
      (void)gtk_file_chooser_set_current_folder_file(chooser, gf, &err);
      g_object_unref(gf);
    } else {
      gchar *dir = g_path_get_dirname(app->scan_root_utf8);
      if (dir != NULL) {
        GFile *gf = g_file_new_for_path(dir);
        (void)gtk_file_chooser_set_current_folder_file(chooser, gf, &err);
        g_object_unref(gf);
        g_free(dir);
      }
    }
    g_clear_error(&err);
  }

  gint resp = gtk_native_dialog_run(GTK_NATIVE_DIALOG(native));
  if (resp == GTK_RESPONSE_ACCEPT) {
    gchar *fn = da_folder_path_from_chooser(chooser);
    if (fn != NULL && fn[0] != '\0') {
      free(app->scan_root_utf8);
      app->scan_root_utf8 = g_strdup(fn);
      g_free(fn);
      scan_controller_notify_scan_root_changed(app);
      da_scan_source_combo_rebuild(app);
    }
  }
  gtk_native_dialog_destroy(GTK_NATIVE_DIALOG(native));
}

void da_scan_source_combo_request_select_folder(AppState *app) {
  if (app == NULL || app->window == NULL || !GTK_IS_WINDOW(app->window)) {
    return;
  }
  run_select_folder_dialog(app);
}

static void configure_open_file_chooser(GtkFileChooser *chooser, AppState *app, int mode) {
  GtkFileFilter *ff = gtk_file_filter_new();
  if (mode == 1) {
    gtk_file_filter_set_name(ff, "CSV");
    gtk_file_filter_add_pattern(ff, "*.csv");
  } else {
    gtk_file_filter_set_name(ff, "MFT Dump");
    gtk_file_filter_add_pattern(ff, "*");
    gtk_file_filter_add_pattern(ff, "*.mft");
    gtk_file_filter_add_pattern(ff, "$MFT");
  }
  gtk_file_chooser_add_filter(chooser, ff);

  if (app->scan_root_utf8 != NULL && app->scan_root_utf8[0] != '\0') {
    gchar *dir = NULL;
    if (g_file_test(app->scan_root_utf8, G_FILE_TEST_IS_DIR)) {
      dir = g_strdup(app->scan_root_utf8);
    } else {
      dir = g_path_get_dirname(app->scan_root_utf8);
    }
    if (dir != NULL) {
      GError *err = NULL;
      GFile *gf = g_file_new_for_path(dir);
      (void)gtk_file_chooser_set_current_folder_file(chooser, gf, &err);
      g_object_unref(gf);
      g_clear_error(&err);
      g_free(dir);
    }
  }
}

void da_scan_source_combo_run_csv_import(AppState *app) {
  if (app == NULL || app->window == NULL || !GTK_IS_WINDOW(app->window)) {
    return;
  }
  GtkFileChooserNative *native =
      gtk_file_chooser_native_new("Select CSV file…", GTK_WINDOW(app->window), GTK_FILE_CHOOSER_ACTION_OPEN,
                                  "_Open", "_Cancel");
  GtkFileChooser *chooser = GTK_FILE_CHOOSER(native);
  configure_open_file_chooser(chooser, app, 1);
  gint resp = gtk_native_dialog_run(GTK_NATIVE_DIALOG(native));
  if (resp == GTK_RESPONSE_ACCEPT) {
    gchar *fn = gtk_file_chooser_get_filename(chooser);
    if (fn != NULL) {
      char err[512];
      scan_result_t *sr = diskatlas_scan_import_csv(fn, err, sizeof err);
      if (sr != NULL) {
        scan_controller_apply_imported_scan(app, sr, fn, TRUE, FALSE);
      } else {
        GtkWidget *d = gtk_message_dialog_new(GTK_WINDOW(app->window), GTK_DIALOG_MODAL, GTK_MESSAGE_ERROR,
                                              GTK_BUTTONS_OK, "%s", err[0] != '\0' ? err : "CSV import failed.");
        da_message_dialog_apply_layout(d);
        gtk_dialog_run(GTK_DIALOG(d));
        gtk_widget_destroy(d);
        da_scan_source_combo_rebuild(app);
      }
      g_free(fn);
    }
  }
  gtk_native_dialog_destroy(GTK_NATIVE_DIALOG(native));
}

static void run_open_file_dialog(AppState *app, gboolean csv_mode) {
  if (csv_mode) {
    da_scan_source_combo_run_csv_import(app);
    return;
  }
  GtkFileChooserNative *native =
      gtk_file_chooser_native_new("Select MFT dump…", GTK_WINDOW(app->window), GTK_FILE_CHOOSER_ACTION_OPEN,
                                  "_Open", "_Cancel");
  GtkFileChooser *chooser = GTK_FILE_CHOOSER(native);
  configure_open_file_chooser(chooser, app, 0);
  gint resp = gtk_native_dialog_run(GTK_NATIVE_DIALOG(native));
  if (resp == GTK_RESPONSE_ACCEPT) {
    gchar *fn = gtk_file_chooser_get_filename(chooser);
    if (fn != NULL) {
#if defined(G_OS_WIN32)
      if (app->scan_root_utf8 == NULL || app->scan_root_utf8[0] == '\0') {
        GtkWidget *d = gtk_message_dialog_new(
            GTK_WINDOW(app->window), GTK_DIALOG_MODAL, GTK_MESSAGE_INFO, GTK_BUTTONS_OK, "%s",
            "Select a drive or folder in the combo first. It must be on the same NTFS volume as this $MFT dump "
            "(used to read the boot sector and to scope paths).");
        da_message_dialog_apply_layout(d);
        gtk_dialog_run(GTK_DIALOG(d));
        gtk_widget_destroy(d);
        da_scan_source_combo_rebuild(app);
      } else {
        char err[512];
        scan_options_t impopt;
        scan_controller_fill_scan_options_for_import(app, &impopt);
        scan_result_t *sr =
            diskatlas_scan_import_raw_mft_file(fn, app->scan_root_utf8, &impopt, err, sizeof err);
        if (sr != NULL) {
          scan_controller_apply_imported_scan(app, sr, fn, TRUE, TRUE);
        } else {
          GtkWidget *d = gtk_message_dialog_new(GTK_WINDOW(app->window), GTK_DIALOG_MODAL, GTK_MESSAGE_ERROR,
                                                GTK_BUTTONS_OK, "%s",
                                                err[0] != '\0' ? err : "MFT dump import failed.");
          da_message_dialog_apply_layout(d);
          gtk_dialog_run(GTK_DIALOG(d));
          gtk_widget_destroy(d);
          da_scan_source_combo_rebuild(app);
        }
      }
#else
      GtkWidget *d = gtk_message_dialog_new(GTK_WINDOW(app->window), GTK_DIALOG_MODAL, GTK_MESSAGE_INFO,
                                            GTK_BUTTONS_OK, "%s",
                                            "Importing a raw NTFS MFT dump is only supported on Windows.");
      da_message_dialog_apply_layout(d);
      gtk_dialog_run(GTK_DIALOG(d));
      gtk_widget_destroy(d);
      da_scan_source_combo_rebuild(app);
#endif
      g_free(fn);
    }
  }
  gtk_native_dialog_destroy(GTK_NATIVE_DIALOG(native));
}

void da_scan_source_combo_setup(AppState *app) {
  GtkListStore *store =
      gtk_list_store_new(DA_SCAN_N_COLS, GDK_TYPE_PIXBUF, G_TYPE_STRING, G_TYPE_STRING, G_TYPE_INT);
  gtk_combo_box_set_model(GTK_COMBO_BOX(app->scan_source_combo), GTK_TREE_MODEL(store));
  g_object_unref(store);

  GtkCellRenderer *pix = gtk_cell_renderer_pixbuf_new();
  GtkCellRenderer *txt = gtk_cell_renderer_text_new();
  gtk_cell_layout_pack_start(GTK_CELL_LAYOUT(app->scan_source_combo), pix, FALSE);
  gtk_cell_layout_add_attribute(GTK_CELL_LAYOUT(app->scan_source_combo), pix, "pixbuf", DA_SCAN_COL_ICON);
  gtk_cell_layout_pack_start(GTK_CELL_LAYOUT(app->scan_source_combo), txt, TRUE);
  gtk_cell_layout_add_attribute(GTK_CELL_LAYOUT(app->scan_source_combo), txt, "text", DA_SCAN_COL_LABEL);

  gtk_widget_set_size_request(app->scan_source_combo, 280, -1);

  da_scan_source_combo_rebuild(app);
}

void da_scan_source_combo_on_changed(GtkComboBox *cb, gpointer user_data) {
  AppState *app = (AppState *)user_data;
  if (app == NULL || g_scan_source_combo_internal) {
    return;
  }

  GtkTreeIter iter;
  if (!gtk_combo_box_get_active_iter(cb, &iter)) {
    return;
  }

  GtkTreeModel *model = gtk_combo_box_get_model(cb);
  gint kind = DA_SCAN_ROW_KIND_PATH;
  gchar *path = NULL;
  gtk_tree_model_get(model, &iter, DA_SCAN_COL_KIND, &kind, DA_SCAN_COL_PATH, &path, -1);

  if (kind == DA_SCAN_ROW_KIND_BROWSE_FOLDER) {
    g_free(path);
    g_scan_source_combo_internal = TRUE;
    gtk_combo_box_set_active(cb, app->scan_source_last_stable_active);
    g_scan_source_combo_internal = FALSE;
    run_select_folder_dialog(app);
    return;
  }
  if (kind == DA_SCAN_ROW_KIND_MFT_FILE) {
    g_free(path);
    g_scan_source_combo_internal = TRUE;
    gtk_combo_box_set_active(cb, app->scan_source_last_stable_active);
    g_scan_source_combo_internal = FALSE;
    run_open_file_dialog(app, FALSE);
    return;
  }
  if (kind == DA_SCAN_ROW_KIND_CSV_FILE) {
    g_free(path);
    g_scan_source_combo_internal = TRUE;
    gtk_combo_box_set_active(cb, app->scan_source_last_stable_active);
    g_scan_source_combo_internal = FALSE;
    da_scan_source_combo_run_csv_import(app);
    return;
  }

  if (path != NULL && path[0] != '\0') {
    if (app->scan_root_utf8 == NULL || g_strcmp0(app->scan_root_utf8, path) != 0) {
      free(app->scan_root_utf8);
      app->scan_root_utf8 = g_strdup(path);
      scan_controller_notify_scan_root_changed(app);
    }
    app->scan_source_last_stable_active = gtk_combo_box_get_active(cb);
  }
  g_free(path);
}
