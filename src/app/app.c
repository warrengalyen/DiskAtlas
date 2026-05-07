#include <stdlib.h>

#include <glib.h>
#include <gtk/gtk.h>

#include "app.h"
#include "diskatlas.h"
#include "volumes.h"
#include "scan_controller.h"
#include "ui_window.h"

static void on_window_destroy(GtkWidget *w, gpointer user_data) {
  (void)w;
  AppState *app = (AppState *)user_data;
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

int diskatlas_app_run(int argc, char **argv) {
  AppState *app = (AppState *)calloc(1, sizeof(AppState));
  if (app == NULL) {
    return 1;
  }

  if (argc > 1 && argv[1] != NULL && argv[1][0] != '\0') {
    app->scan_root_utf8 = g_strdup(argv[1]);
  } else {
    app->scan_root_utf8 = da_volume_system_root_utf8();
    if (app->scan_root_utf8 == NULL) {
      const gchar *h = g_get_home_dir();
      app->scan_root_utf8 = g_strdup(h != NULL ? h : ".");
    }
  }

  app->gtk_app = gtk_application_new("com.diskatlas.DiskAtlas", G_APPLICATION_NON_UNIQUE);
  g_signal_connect(app->gtk_app, "activate", G_CALLBACK(activate), app);
  int status = g_application_run(G_APPLICATION(app->gtk_app), argc, argv);

  g_object_unref(app->gtk_app);
  free(app->master_indices);
  free(app->filtered_indices);
  g_free(app->scan_root_utf8);
  free(app);
  return status;
}
