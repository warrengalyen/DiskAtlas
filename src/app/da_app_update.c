#include "da_app_update.h"

#include <gio/gio.h>
#include <glib.h>
#include <gtk/gtk.h>
#include <string.h>

#include "app_state.h"
#include "da_message_dialog.h"
#include "da_update_config.h"

#if DISKATLAS_HAVE_LIBUPDATE
#include <update.h>
#endif

enum {
  DA_UP_NOT_CONFIGURED = -4,
  DA_UP_INIT_FAIL = -2,
  DA_UP_BAD_CHECKSUM = -5,
  /** Download finished; main thread must call update_apply after user prompt. */
  DA_UP_DOWNLOADED_OK = 100,
};

static void da_up_show_message(GtkWindow *parent, GtkMessageType mt,
                               const char *text) {
  if (parent == NULL || text == NULL) {
    return;
  }
  GtkWidget *d = gtk_message_dialog_new(parent, GTK_DIALOG_MODAL, mt,
                                        GTK_BUTTONS_OK, "%s", text);
  da_message_dialog_apply_layout(d);
  gtk_dialog_run(GTK_DIALOG(d));
  gtk_widget_destroy(d);
}

#if DISKATLAS_HAVE_LIBUPDATE

typedef struct {
  gint status;
  update_info_t info;
} DaUpCheckResult;

typedef struct {
  AppState *app;
  GtkWidget *progress_dialog;
  GtkWidget *progress_bar;
  GtkWidget *progress_label;
  GMutex prog_mutex;
  volatile gint prog_idle_pending;
  guint64 prog_done;
  guint64 prog_total;
  gchar *workdir;
  gchar *zip_path;
} DaUpDownloadJob;

/** libupdate requires a 64-character lowercase/uppercase hex SHA-256 in the
 * manifest. */
static gboolean da_up_manifest_checksum_format_ok(const char *s) {
  size_t i, n;
  if (s == NULL) {
    return FALSE;
  }
  n = strlen(s);
  if (n != 64) {
    return FALSE;
  }
  for (i = 0; i < n; i++) {
    if (!g_ascii_isxdigit((guchar)s[i])) {
      return FALSE;
    }
  }
  return TRUE;
}

static gboolean da_up_prog_flush_idle(gpointer user_data) {
  DaUpDownloadJob *job = (DaUpDownloadJob *)user_data;
  guint64 done;
  guint64 total;
  gchar *line;

  g_atomic_int_set(&job->prog_idle_pending, 0);

  g_mutex_lock(&job->prog_mutex);
  done = job->prog_done;
  total = job->prog_total;
  g_mutex_unlock(&job->prog_mutex);

  if (job->progress_bar == NULL || !GTK_IS_WIDGET(job->progress_bar)) {
    return G_SOURCE_REMOVE;
  }

  if (total > 0ULL) {
    gdouble frac = (gdouble)done / (gdouble)total;
    if (frac > 1.0) {
      frac = 1.0;
    }
    gtk_progress_bar_set_fraction(GTK_PROGRESS_BAR(job->progress_bar), frac);
  } else {
    gtk_progress_bar_pulse(GTK_PROGRESS_BAR(job->progress_bar));
  }

  if (job->progress_label != NULL && GTK_IS_WIDGET(job->progress_label)) {
    if (total > 0ULL) {
      gchar *a = g_format_size_full((guint64)done, G_FORMAT_SIZE_DEFAULT);
      gchar *b = g_format_size_full((guint64)total, G_FORMAT_SIZE_DEFAULT);
      line = g_strdup_printf("%s of %s", a, b);
      g_free(a);
      g_free(b);
    } else {
      gchar *a = g_format_size_full((guint64)done, G_FORMAT_SIZE_DEFAULT);
      line = g_strdup_printf("%s downloaded", a);
      g_free(a);
    }
    gtk_label_set_text(GTK_LABEL(job->progress_label), line);
    g_free(line);
  }

  return G_SOURCE_REMOVE;
}

static void da_up_download_progress_cb(unsigned long long bytes_done,
                                       unsigned long long bytes_total_hint,
                                       void *user_data) {
  DaUpDownloadJob *job = (DaUpDownloadJob *)user_data;

  g_mutex_lock(&job->prog_mutex);
  job->prog_done = bytes_done;
  job->prog_total = bytes_total_hint;
  g_mutex_unlock(&job->prog_mutex);

  if (g_atomic_int_compare_and_exchange(&job->prog_idle_pending, 0, 1)) {
    g_idle_add(da_up_prog_flush_idle, job);
  }
}

static void da_up_check_worker(GTask *task, gpointer source_object,
                               gpointer task_data, GCancellable *cancellable) {
  (void)source_object;
  (void)task_data;
  (void)cancellable;

  DaUpCheckResult *r = g_new0(DaUpCheckResult, 1);

  if (DA_UPDATE_MANIFEST_URL[0] == '\0') {
    r->status = DA_UP_NOT_CONFIGURED;
    g_task_return_pointer(task, r, g_free);
    return;
  }

  update_options_t opts = {0};
  opts.update_url = DA_UPDATE_MANIFEST_URL;
  opts.app_name = "diskatlas";

  if (update_init(&opts) != UPDATE_OK) {
    r->status = DA_UP_INIT_FAIL;
    g_task_return_pointer(task, r, g_free);
    return;
  }

  memset(&r->info, 0, sizeof r->info);
  int st = update_check(&r->info);
  update_shutdown();

  if (st == UPDATE_ERROR) {
    r->status = UPDATE_ERROR;
    g_task_return_pointer(task, r, g_free);
    return;
  }

  if (st == UPDATE_NOT_AVAILABLE) {
    r->status = UPDATE_NOOP;
    g_task_return_pointer(task, r, g_free);
    return;
  }

  if (st != UPDATE_AVAILABLE) {
    r->status = UPDATE_ERROR;
    g_task_return_pointer(task, r, g_free);
    return;
  }

  if (!da_up_manifest_checksum_format_ok(r->info.checksum)) {
    r->status = DA_UP_BAD_CHECKSUM;
    g_task_return_pointer(task, r, g_free);
    return;
  }

  if (r->info.download_url[0] == '\0') {
    r->status = UPDATE_ERROR;
    g_task_return_pointer(task, r, g_free);
    return;
  }

  r->status = UPDATE_AVAILABLE;
  g_task_return_pointer(task, r, g_free);
}

static void da_up_download_job_free(DaUpDownloadJob *job) {
  if (job == NULL) {
    return;
  }
  g_mutex_lock(&job->prog_mutex);
  job->progress_bar = NULL;
  job->progress_label = NULL;
  g_mutex_unlock(&job->prog_mutex);
  g_mutex_clear(&job->prog_mutex);
  if (job->workdir != NULL) {
    (void)update_remove_tree(job->workdir);
    g_free(job->workdir);
  }
  g_free(job->zip_path);
  g_free(job);
}

static void da_up_download_worker(GTask *task, gpointer source_object,
                                  gpointer task_data, GCancellable *cancellable) {
  (void)source_object;
  (void)cancellable;
  DaUpDownloadJob *job = (DaUpDownloadJob *)task_data;
  const gchar *url = (const gchar *)g_object_get_data(G_OBJECT(task), "da-up-url");
  const gchar *sha = (const gchar *)g_object_get_data(G_OBJECT(task), "da-up-sha");
  update_options_t opts = {0};

  opts.update_url = DA_UPDATE_MANIFEST_URL;
  opts.app_name = "diskatlas";
  opts.expected_sha256 = sha;

  if (update_init(&opts) != UPDATE_OK) {
    g_task_return_int(task, DA_UP_INIT_FAIL);
    return;
  }

  update_set_download_progress_callback(da_up_download_progress_cb, job);

  if (update_download(url, job->zip_path) != UPDATE_OK) {
    update_shutdown();
    g_task_return_int(task, UPDATE_ERROR);
    return;
  }

  update_set_download_progress_callback(NULL, NULL);

  /* Leave libupdate initialized; main thread shows a prompt then update_apply (exit). */
  g_task_return_int(task, DA_UP_DOWNLOADED_OK);
}

static void da_up_download_complete(GObject *source_object, GAsyncResult *res,
                                    gpointer user_data) {
  (void)source_object;
  DaUpDownloadJob *job = (DaUpDownloadJob *)user_data;
  AppState *app = job->app;
  GError *err = NULL;
  gint st = g_task_propagate_int(G_TASK(res), &err);

  g_mutex_lock(&job->prog_mutex);
  job->progress_bar = NULL;
  job->progress_label = NULL;
  g_mutex_unlock(&job->prog_mutex);

  if (job->progress_dialog != NULL && GTK_IS_WIDGET(job->progress_dialog)) {
    gtk_widget_destroy(job->progress_dialog);
    job->progress_dialog = NULL;
  }

  if (app->window != NULL) {
    gtk_widget_set_sensitive(app->window, TRUE);
  }

  if (err != NULL) {
    if (app->window != NULL) {
      gchar *msg = g_strdup_printf(
          "The update download did not finish.\n\n"
          "Please try again in a moment. If you keep seeing this message, check "
          "your internet connection or install DiskAtlas using the full installer "
          "from the website.\n\n"
          "Details: %s",
          err->message);
      if (msg != NULL) {
        da_up_show_message(GTK_WINDOW(app->window), GTK_MESSAGE_ERROR, msg);
        g_free(msg);
      }
    }
    g_clear_error(&err);
    da_up_download_job_free(job);
    return;
  }

  if (app->window == NULL) {
    da_up_download_job_free(job);
    return;
  }

  GtkWindow *w = GTK_WINDOW(app->window);

  switch (st) {
  case DA_UP_INIT_FAIL:
    da_up_show_message(
        w, GTK_MESSAGE_ERROR,
        "DiskAtlas could not prepare the download step.\n\n"
        "Try closing the application completely and opening it again, then use "
        "Help → Check for Updates once more.");
    break;
  case DA_UP_DOWNLOADED_OK: {
    GtkWidget *d = gtk_message_dialog_new(
        w, GTK_DIALOG_MODAL, GTK_MESSAGE_INFO, GTK_BUTTONS_OK, "%s",
        "The update has been downloaded. DiskAtlas will close so the updater can "
        "install it. Save any work before continuing.");
    da_message_dialog_apply_layout(d);
    gtk_dialog_run(GTK_DIALOG(d));
    gtk_widget_destroy(d);
    if (update_apply(job->zip_path) != UPDATE_OK) {
      update_shutdown();
      da_up_show_message(
          w, GTK_MESSAGE_ERROR,
          "DiskAtlas could not start the updater program, so the update was not "
          "installed.\n\n"
          "Try closing other copies of DiskAtlas, run the app as a normal user "
          "(not from a restricted folder), and try Help → Check for Updates "
          "again. You can also install the latest version using the full "
          "installer from the publisher's website.");
    }
    /* update_apply success: exit(0) inside libupdate; not reached */
    break;
  }
  case UPDATE_ERROR:
  default:
    da_up_show_message(
        w, GTK_MESSAGE_ERROR,
        "The update file could not be downloaded completely, or it did not match "
        "what the update server published.\n\n"
        "Check your internet connection and try Help → Check for Updates again. "
        "If the problem continues, download the latest DiskAtlas package manually "
        "from the website.");
    break;
  }

  da_up_download_job_free(job);
}

static void da_up_start_download(AppState *app, const update_info_t *info) {
  DaUpDownloadJob *job = g_new0(DaUpDownloadJob, 1);
  GTask *task;
  GtkWidget *content;
  GtkWidget *box;
  gchar *sha_copy;
  gchar *url_copy;

  job->app = app;
  g_mutex_init(&job->prog_mutex);
  job->prog_idle_pending = 0;

  job->workdir = g_dir_make_tmp("diskatlas_up_XXXXXX", NULL);
  if (job->workdir == NULL) {
    da_up_show_message(GTK_WINDOW(app->window), GTK_MESSAGE_ERROR,
                       "Could not create a temporary folder for the update download.");
    g_mutex_clear(&job->prog_mutex);
    g_free(job);
    gtk_widget_set_sensitive(app->window, TRUE);
    return;
  }

  job->zip_path = g_build_filename(job->workdir, "package.bin", NULL);

  job->progress_dialog =
      gtk_dialog_new_with_buttons("Downloading update", GTK_WINDOW(app->window),
                                GTK_DIALOG_MODAL | GTK_DIALOG_DESTROY_WITH_PARENT,
                                NULL);
  gtk_window_set_default_size(GTK_WINDOW(job->progress_dialog), 420, -1);
  da_message_dialog_apply_layout(job->progress_dialog);

  box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);
  gtk_widget_set_margin_top(box, 12);
  gtk_widget_set_margin_bottom(box, 12);
  gtk_widget_set_margin_start(box, 14);
  gtk_widget_set_margin_end(box, 14);

  job->progress_label = gtk_label_new("Starting…");
  gtk_label_set_xalign(GTK_LABEL(job->progress_label), 0.0f);
  gtk_widget_set_hexpand(job->progress_label, TRUE);
  gtk_box_pack_start(GTK_BOX(box), job->progress_label, FALSE, TRUE, 0);

  job->progress_bar = gtk_progress_bar_new();
  gtk_progress_bar_set_show_text(GTK_PROGRESS_BAR(job->progress_bar), FALSE);
  gtk_box_pack_start(GTK_BOX(box), job->progress_bar, FALSE, TRUE, 0);

  content = gtk_dialog_get_content_area(GTK_DIALOG(job->progress_dialog));
  gtk_container_add(GTK_CONTAINER(content), box);
  gtk_widget_show_all(job->progress_dialog);

  gtk_widget_set_sensitive(app->window, FALSE);

  sha_copy = g_strdup(info->checksum);
  url_copy = g_strdup(info->download_url);

  task = g_task_new(G_OBJECT(app->window), NULL, da_up_download_complete, job);
  g_object_set_data_full(G_OBJECT(task), "da-up-sha", sha_copy, g_free);
  g_object_set_data_full(G_OBJECT(task), "da-up-url", url_copy, g_free);
  g_task_set_task_data(task, job, NULL);
  g_task_run_in_thread(task, da_up_download_worker);
  g_object_unref(task);
}

static void da_up_check_complete(GObject *source_object, GAsyncResult *res,
                                 gpointer user_data) {
  (void)source_object;
  AppState *app = (AppState *)user_data;
  GError *err = NULL;
  DaUpCheckResult *r = (DaUpCheckResult *)g_task_propagate_pointer(G_TASK(res), &err);

  if (app->window != NULL) {
    gtk_widget_set_sensitive(app->window, TRUE);
  }

  if (err != NULL) {
    if (app->window != NULL) {
      gchar *msg = g_strdup_printf(
          "We couldn't check whether an update is available.\n\n"
          "Please try again shortly. If this keeps happening, make sure you are "
          "online and that firewall or proxy settings allow DiskAtlas to reach "
          "the update server.\n\n"
          "Details: %s",
          err->message);
      if (msg != NULL) {
        da_up_show_message(GTK_WINDOW(app->window), GTK_MESSAGE_ERROR, msg);
        g_free(msg);
      }
    }
    g_clear_error(&err);
    return;
  }

  if (r == NULL || app->window == NULL) {
    g_free(r);
    return;
  }

  GtkWindow *w = GTK_WINDOW(app->window);

  switch (r->status) {
  case DA_UP_NOT_CONFIGURED:
    da_up_show_message(
        w, GTK_MESSAGE_INFO,
        "Automatic update checking is turned off for this build.\n\n"
        "Your administrator can enable it by setting DISKATLAS_UPDATE_MANIFEST_URL "
        "when configuring the application.");
    break;
  case DA_UP_INIT_FAIL:
    da_up_show_message(
        w, GTK_MESSAGE_ERROR,
        "DiskAtlas could not prepare the built-in update feature.\n\n"
        "Try closing the application completely and opening it again. If the "
        "problem continues, install updates using the full installer from the "
        "publisher's website.");
    break;
  case UPDATE_NOOP:
  case UPDATE_NOT_AVAILABLE:
    da_up_show_message(w, GTK_MESSAGE_INFO,
                       "You are running the latest version of DiskAtlas.");
    break;
  case DA_UP_BAD_CHECKSUM:
    da_up_show_message(
        w, GTK_MESSAGE_ERROR,
        "The update information on the server looks invalid (the security "
        "checksum is not in the expected format).\n\n"
        "Nothing was downloaded. Please try again later, or contact the "
        "publisher if the problem continues.");
    break;
  case UPDATE_AVAILABLE: {
    gchar *body = g_strdup_printf(
        "A newer version of DiskAtlas is available (version %s).\n\n"
        "Download this update now?",
        r->info.version[0] != '\0' ? r->info.version : "(unknown)");
    GtkWidget *d = gtk_message_dialog_new(
        w, GTK_DIALOG_MODAL | GTK_DIALOG_DESTROY_WITH_PARENT, GTK_MESSAGE_QUESTION,
        GTK_BUTTONS_YES_NO, "%s", body != NULL ? body : "Download update?");
    g_free(body);
    gtk_dialog_set_default_response(GTK_DIALOG(d), GTK_RESPONSE_NO);
    da_message_dialog_apply_layout(d);
    gint response = gtk_dialog_run(GTK_DIALOG(d));
    gtk_widget_destroy(d);

    if (response == GTK_RESPONSE_YES) {
      da_up_start_download(app, &r->info);
    }
    break;
  }
  case UPDATE_ERROR:
  default:
    da_up_show_message(
        w, GTK_MESSAGE_ERROR,
        "DiskAtlas could not read the update information from the server.\n\n"
        "You may be offline, the update service may be busy, or the published "
        "update file may be misconfigured. Try again in a few minutes, or "
        "download the latest version from the publisher's website if you need "
        "the update right away.");
    break;
  }

  g_free(r);
}

#endif /* DISKATLAS_HAVE_LIBUPDATE */

void da_help_menu_check_for_updates(GtkMenuItem *item, gpointer user_data) {
  (void)item;
  AppState *app = (AppState *)user_data;
  if (app == NULL || app->window == NULL) {
    return;
  }

#if !DISKATLAS_HAVE_LIBUPDATE
  da_up_show_message(
      GTK_WINDOW(app->window), GTK_MESSAGE_INFO,
      "This build was not linked with the update library.");
#else
  gtk_widget_set_sensitive(app->window, FALSE);

  GTask *task = g_task_new(G_OBJECT(app->window), NULL, da_up_check_complete, app);
  g_task_run_in_thread(task, da_up_check_worker);
  g_object_unref(task);
#endif
}
