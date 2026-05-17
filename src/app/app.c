#include <stdlib.h>

#include <glib.h>
#include <gtk/gtk.h>

#include <string.h>

#include "app.h"
#include "diskatlas.h"
#include "volumes.h"
#include "diskatlas_ini.h"
#include "dm_mime_db.h"
#include "scan_controller.h"
#include "ui_window.h"

static void on_window_destroy(GtkWidget *w, gpointer user_data) {
  (void)w;
  AppState *app = (AppState *)user_data;
  da_ini_save_interface(app);
  da_ini_save_filetree(app);
  scan_controller_detach(app);
  if (app->scan != NULL) {
    scan_result_free(app->scan);
    app->scan = NULL;
  }
}

static void activate(GtkApplication *gtk_app, gpointer user_data) {
  (void)gtk_app;
  AppState *app = (AppState *)user_data;
  da_ui_build(app);
  g_signal_connect(app->window, "destroy", G_CALLBACK(on_window_destroy), app);
}

static gboolean da_argv_has_token(int argc, char **argv, const char *token) {
  for (int i = 1; i < argc; i++) {
    if (argv[i] != NULL && strcmp(argv[i], token) == 0) {
      return TRUE;
    }
  }
  return FALSE;
}

static void da_argv_remove_token(int *argc, char **argv, const char *token) {
  int w = 1;
  for (int r = 1; r < *argc; r++) {
    if (argv[r] != NULL && strcmp(argv[r], token) == 0) {
      continue;
    }
    argv[w++] = argv[r];
  }
  argv[w] = NULL;
  *argc = w;
}

int diskatlas_app_run(int argc, char **argv) {
  AppState *app = (AppState *)calloc(1, sizeof(AppState));
  if (app == NULL) {
    return 1;
  }

  gboolean launched_for_elevation = da_argv_has_token(argc, argv, "--elevated");
  da_argv_remove_token(&argc, argv, "--elevated");

  if (argc > 1 && argv[1] != NULL && argv[1][0] != '\0') {
    app->scan_root_utf8 = g_strdup(argv[1]);
  } else {
    app->scan_root_utf8 = da_volume_system_root_utf8();
    if (app->scan_root_utf8 == NULL) {
      const gchar *h = g_get_home_dir();
      app->scan_root_utf8 = g_strdup(h != NULL ? h : ".");
    }
  }

  da_ini_load_general(app);

#if defined(G_OS_WIN32)
  if (app->general_always_run_as_admin && !da_win32_is_process_elevated() && !launched_for_elevation) {
    if (da_win32_restart_elevated_self(TRUE)) {
      g_free(app->scan_root_utf8);
      free(app);
      return 0;
    }
  }
#endif

  GPtrArray *ini_cats = da_ini_mime_categories_load();
  app->mime_db = dm_mime_db_build(ini_cats);
  g_ptr_array_unref(ini_cats);

  app->gtk_app = gtk_application_new("com.diskatlas.DiskAtlas", G_APPLICATION_NON_UNIQUE);
  g_signal_connect(app->gtk_app, "activate", G_CALLBACK(activate), app);
  int status = g_application_run(G_APPLICATION(app->gtk_app), argc, argv);

  dm_mime_db_free(app->mime_db);
  app->mime_db = NULL;
  g_object_unref(app->gtk_app);
  if (app->search_history != NULL) {
    g_ptr_array_unref(app->search_history);
    app->search_history = NULL;
  }
  free(app->master_indices);
  free(app->filtered_indices);
  g_free(app->scan_root_utf8);
  g_free(app->csv_import_path);
  g_free(app->csv_derived_root_utf8);
  g_free(app->mft_dump_save_path);
  g_free(app->mft_dump_volume_root_utf8);
  free(app);
  return status;
}
