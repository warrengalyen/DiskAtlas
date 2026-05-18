#include "gtk_libupdate.h"

#include <gio/gio.h>
#include <glib.h>
#include <string.h>

#ifndef GTK_LIBUPDATE_HAVE_LIBUPDATE
#define GTK_LIBUPDATE_HAVE_LIBUPDATE 0
#endif

#if GTK_LIBUPDATE_HAVE_LIBUPDATE
#include <update.h>

enum {
  GTK_LU_NOT_CONFIGURED = -4,
  GTK_LU_INIT_FAIL = -2,
  GTK_LU_BAD_CHECKSUM = -5,
  GTK_LU_DOWNLOADED_OK = 100,
};

typedef struct {
  GtkWindow *parent;
  GtkWidget *main_widget;
  GtkLibupdateConfig cfg_copy;
} GtkLuCheckUserData;

typedef struct {
  gint status;
  update_info_t info;
} GtkLuCheckResult;

typedef struct {
  GtkWindow *parent;
  GtkWidget *main_widget;
  /** Copy of caller config; must not point into freed stack/heap (by-value copy of struct). */
  GtkLibupdateConfig cfg;
  GtkWidget *progress_dialog;
  GtkWidget *progress_bar;
  GtkWidget *progress_label;
  GMutex prog_mutex;
  volatile gint prog_idle_pending;
  guint64 prog_done;
  guint64 prog_total;
  gchar *workdir;
  gchar *zip_path;
} GtkLuDownloadJob;

static void gtk_lu_apply_dialog_layout(const GtkLibupdateConfig *cfg, GtkWidget *dialog) {
  if (cfg != NULL && cfg->configure_message_dialog != NULL && dialog != NULL) {
    cfg->configure_message_dialog(dialog);
  }
}

static gboolean gtk_lu_manifest_checksum_format_ok(const char *s) {
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

static gboolean gtk_lu_prog_flush_idle(gpointer user_data) {
  GtkLuDownloadJob *job = (GtkLuDownloadJob *)user_data;
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

static void gtk_lu_download_progress_cb(unsigned long long bytes_done,
                                        unsigned long long bytes_total_hint,
                                        void *user_data) {
  GtkLuDownloadJob *job = (GtkLuDownloadJob *)user_data;

  g_mutex_lock(&job->prog_mutex);
  job->prog_done = bytes_done;
  job->prog_total = bytes_total_hint;
  g_mutex_unlock(&job->prog_mutex);

  if (g_atomic_int_compare_and_exchange(&job->prog_idle_pending, 0, 1)) {
    g_idle_add(gtk_lu_prog_flush_idle, job);
  }
}

static void gtk_lu_check_worker(GTask *task, gpointer source_object, gpointer task_data,
                                GCancellable *cancellable) {
  (void)source_object;
  (void)task_data;
  (void)cancellable;

  const GtkLibupdateConfig *cfg =
      (const GtkLibupdateConfig *)g_object_get_data(G_OBJECT(task), "gtk-lu-cfg");
  GtkLuCheckResult *r = g_new0(GtkLuCheckResult, 1);

  if (cfg == NULL || cfg->manifest_url == NULL || cfg->manifest_url[0] == '\0') {
    r->status = GTK_LU_NOT_CONFIGURED;
    g_task_return_pointer(task, r, g_free);
    return;
  }

  update_options_t opts = {0};
  opts.update_url = cfg->manifest_url;
  opts.app_name = cfg->libupdate_app_name;

  if (update_init(&opts) != UPDATE_OK) {
    r->status = GTK_LU_INIT_FAIL;
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

  if (st == UPDATE_NOT_AVAILABLE || st == UPDATE_NOOP) {
    r->status = UPDATE_NOOP;
    g_task_return_pointer(task, r, g_free);
    return;
  }

  if (st != UPDATE_AVAILABLE) {
    r->status = UPDATE_ERROR;
    g_task_return_pointer(task, r, g_free);
    return;
  }

  if (!gtk_lu_manifest_checksum_format_ok(r->info.checksum)) {
    r->status = GTK_LU_BAD_CHECKSUM;
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

static void gtk_lu_download_job_free(GtkLuDownloadJob *job) {
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

static void gtk_lu_download_worker(GTask *task, gpointer source_object, gpointer task_data,
                                   GCancellable *cancellable) {
  (void)source_object;
  (void)cancellable;
  GtkLuDownloadJob *job = (GtkLuDownloadJob *)task_data;
  const gchar *url = (const gchar *)g_object_get_data(G_OBJECT(task), "gtk-lu-url");
  const gchar *sha = (const gchar *)g_object_get_data(G_OBJECT(task), "gtk-lu-sha");
  const GtkLibupdateConfig *cfg = &job->cfg;
  update_options_t opts = {0};

  opts.update_url = cfg->manifest_url;
  opts.app_name = cfg->libupdate_app_name;
  opts.expected_sha256 = sha;

  if (update_init(&opts) != UPDATE_OK) {
    g_task_return_int(task, GTK_LU_INIT_FAIL);
    return;
  }

  update_set_download_progress_callback(gtk_lu_download_progress_cb, job);

  if (update_download(url, job->zip_path) != UPDATE_OK) {
    update_shutdown();
    g_task_return_int(task, UPDATE_ERROR);
    return;
  }

  update_set_download_progress_callback(NULL, NULL);

  g_task_return_int(task, GTK_LU_DOWNLOADED_OK);
}

static void gtk_lu_download_complete(GObject *source_object, GAsyncResult *res, gpointer user_data) {
  (void)source_object;
  GtkLuDownloadJob *job = (GtkLuDownloadJob *)user_data;
  const GtkLibupdateConfig *cfg = &job->cfg;
  const char *name = (cfg != NULL && cfg->display_name != NULL) ? cfg->display_name : "This application";
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

  if (job->main_widget != NULL) {
    gtk_widget_set_sensitive(job->main_widget, TRUE);
  }

  if (err != NULL) {
    if (job->parent != NULL) {
      gchar *msg = g_strdup_printf(
          "The update download did not finish.\n\n"
          "Please try again in a moment. If you keep seeing this message, check "
          "your internet connection or install %s using the full installer "
          "from the website.\n\n"
          "Details: %s",
          name, err->message);
      if (msg != NULL) {
        GtkWidget *d = gtk_message_dialog_new(job->parent, GTK_DIALOG_MODAL, GTK_MESSAGE_ERROR,
                                              GTK_BUTTONS_OK, "%s", msg);
        gtk_lu_apply_dialog_layout(cfg, d);
        gtk_dialog_run(GTK_DIALOG(d));
        gtk_widget_destroy(d);
        g_free(msg);
      }
    }
    g_clear_error(&err);
    gtk_lu_download_job_free(job);
    return;
  }

  if (job->parent == NULL) {
    gtk_lu_download_job_free(job);
    return;
  }

  GtkWindow *w = job->parent;

  switch (st) {
  case GTK_LU_INIT_FAIL: {
    gchar *msg = g_strdup_printf(
        "%s could not prepare the download step.\n\n"
        "Try closing the application completely and opening it again, then check "
        "for updates once more.",
        name);
    if (msg != NULL) {
      GtkWidget *d = gtk_message_dialog_new(w, GTK_DIALOG_MODAL, GTK_MESSAGE_ERROR, GTK_BUTTONS_OK,
                                            "%s", msg);
      gtk_lu_apply_dialog_layout(cfg, d);
      gtk_dialog_run(GTK_DIALOG(d));
      gtk_widget_destroy(d);
      g_free(msg);
    }
    break;
  }
  case GTK_LU_DOWNLOADED_OK: {
    gchar *msg = g_strdup_printf(
        "The update has been downloaded. %s will close so the updater can "
        "install it. Save any work before continuing.",
        name);
    GtkWidget *d = gtk_message_dialog_new(w, GTK_DIALOG_MODAL, GTK_MESSAGE_INFO, GTK_BUTTONS_OK, "%s",
                                          msg != NULL ? msg : "");
    g_free(msg);
    gtk_lu_apply_dialog_layout(cfg, d);
    gtk_dialog_run(GTK_DIALOG(d));
    gtk_widget_destroy(d);
    if (update_apply(job->zip_path) != UPDATE_OK) {
      update_shutdown();
      gchar *errm = g_strdup_printf(
          "%s could not start the updater program, so the update was not "
          "installed.\n\n"
          "Try closing other copies of the program, run the app as a normal user "
          "(not from a restricted folder), and try checking for updates "
          "again. You can also install the latest version using the full "
          "installer from the publisher's website.",
          name);
      if (errm != NULL) {
        GtkWidget *ed = gtk_message_dialog_new(w, GTK_DIALOG_MODAL, GTK_MESSAGE_ERROR, GTK_BUTTONS_OK,
                                               "%s", errm);
        gtk_lu_apply_dialog_layout(cfg, ed);
        gtk_dialog_run(GTK_DIALOG(ed));
        gtk_widget_destroy(ed);
        g_free(errm);
      }
    }
    break;
  }
  case UPDATE_ERROR:
  default: {
    gchar *msg = g_strdup_printf(
        "The update file could not be downloaded completely, or it did not match "
        "what the update server published.\n\n"
        "Check your internet connection and try again. "
        "If the problem continues, download the latest %s package manually "
        "from the website.",
        name);
    if (msg != NULL) {
      GtkWidget *d = gtk_message_dialog_new(w, GTK_DIALOG_MODAL, GTK_MESSAGE_ERROR, GTK_BUTTONS_OK,
                                            "%s", msg);
      gtk_lu_apply_dialog_layout(cfg, d);
      gtk_dialog_run(GTK_DIALOG(d));
      gtk_widget_destroy(d);
      g_free(msg);
    }
    break;
  }
  }

  gtk_lu_download_job_free(job);
}

static void gtk_lu_start_download(GtkWindow *parent, GtkWidget *main_widget, const GtkLibupdateConfig *cfg,
                                  const update_info_t *info) {
  GtkLuDownloadJob *job = g_new0(GtkLuDownloadJob, 1);
  GTask *task;
  GtkWidget *content;
  GtkWidget *box;
  gchar *sha_copy;
  gchar *url_copy;

  job->parent = parent;
  job->main_widget = main_widget;
  job->cfg = *cfg;
  g_mutex_init(&job->prog_mutex);
  job->prog_idle_pending = 0;

  job->workdir = g_dir_make_tmp(cfg->temp_dir_template, NULL);
  if (job->workdir == NULL) {
    GtkWidget *d = gtk_message_dialog_new(
        parent, GTK_DIALOG_MODAL, GTK_MESSAGE_ERROR, GTK_BUTTONS_OK,
        "Could not create a temporary folder for the update download.");
    gtk_lu_apply_dialog_layout(cfg, d);
    gtk_dialog_run(GTK_DIALOG(d));
    gtk_widget_destroy(d);
    g_mutex_clear(&job->prog_mutex);
    g_free(job);
    gtk_widget_set_sensitive(main_widget, TRUE);
    return;
  }

  job->zip_path = g_build_filename(job->workdir, "package.bin", NULL);

  job->progress_dialog =
      gtk_dialog_new_with_buttons("Downloading update", parent,
                                  GTK_DIALOG_MODAL | GTK_DIALOG_DESTROY_WITH_PARENT, NULL);
  gtk_window_set_default_size(GTK_WINDOW(job->progress_dialog), 420, -1);
  gtk_lu_apply_dialog_layout(cfg, job->progress_dialog);

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

  gtk_widget_set_sensitive(main_widget, FALSE);

  sha_copy = g_strdup(info->checksum);
  url_copy = g_strdup(info->download_url);

  task = g_task_new(G_OBJECT(parent), NULL, gtk_lu_download_complete, job);
  g_object_set_data_full(G_OBJECT(task), "gtk-lu-sha", sha_copy, g_free);
  g_object_set_data_full(G_OBJECT(task), "gtk-lu-url", url_copy, g_free);
  g_task_set_task_data(task, job, NULL);
  g_task_run_in_thread(task, gtk_lu_download_worker);
  g_object_unref(task);
}

static void gtk_lu_check_complete_fixed(GObject *source_object, GAsyncResult *res, gpointer user_data) {
  (void)source_object;
  GtkLuCheckUserData *ud = (GtkLuCheckUserData *)user_data;
  GtkWindow *parent = ud->parent;
  GtkWidget *main_widget = ud->main_widget;
  const GtkLibupdateConfig *cfg = &ud->cfg_copy;
  const char *name = (cfg->display_name != NULL) ? cfg->display_name : "This application";
  GError *err = NULL;
  GtkLuCheckResult *r = (GtkLuCheckResult *)g_task_propagate_pointer(G_TASK(res), &err);

  if (main_widget != NULL) {
    gtk_widget_set_sensitive(main_widget, TRUE);
  }

  if (err != NULL) {
    if (parent != NULL) {
      gchar *msg = g_strdup_printf(
          "We couldn't check whether an update is available.\n\n"
          "Please try again shortly. If this keeps happening, make sure you are "
          "online and that firewall or proxy settings allow %s to reach "
          "the update server.\n\n"
          "Details: %s",
          name, err->message);
      if (msg != NULL) {
        GtkWidget *d = gtk_message_dialog_new(parent, GTK_DIALOG_MODAL, GTK_MESSAGE_ERROR, GTK_BUTTONS_OK,
                                              "%s", msg);
        gtk_lu_apply_dialog_layout(cfg, d);
        gtk_dialog_run(GTK_DIALOG(d));
        gtk_widget_destroy(d);
        g_free(msg);
      }
    }
    g_clear_error(&err);
    g_free(ud);
    return;
  }

  if (r == NULL || parent == NULL) {
    g_free(r);
    g_free(ud);
    return;
  }

  GtkWindow *w = parent;

  switch (r->status) {
  case GTK_LU_NOT_CONFIGURED: {
    const char *extra = cfg->disabled_build_explanation;
    gchar *msg;
    if (extra != NULL && extra[0] != '\0') {
      msg = g_strdup_printf("Automatic update checking is turned off for this build.\n\n%s", extra);
    } else {
      msg = g_strdup("Automatic update checking is turned off for this build.");
    }
    if (msg != NULL) {
      GtkWidget *d = gtk_message_dialog_new(w, GTK_DIALOG_MODAL, GTK_MESSAGE_INFO, GTK_BUTTONS_OK, "%s",
                                            msg);
      gtk_lu_apply_dialog_layout(cfg, d);
      gtk_dialog_run(GTK_DIALOG(d));
      gtk_widget_destroy(d);
      g_free(msg);
    }
    break;
  }
  case GTK_LU_INIT_FAIL: {
    gchar *msg = g_strdup_printf(
        "%s could not prepare the built-in update feature.\n\n"
        "Try closing the application completely and opening it again. If the "
        "problem continues, install updates using the full installer from the "
        "publisher's website.",
        name);
    if (msg != NULL) {
      GtkWidget *d = gtk_message_dialog_new(w, GTK_DIALOG_MODAL, GTK_MESSAGE_ERROR, GTK_BUTTONS_OK, "%s",
                                            msg);
      gtk_lu_apply_dialog_layout(cfg, d);
      gtk_dialog_run(GTK_DIALOG(d));
      gtk_widget_destroy(d);
      g_free(msg);
    }
    break;
  }
  case UPDATE_NOOP:
  case UPDATE_NOT_AVAILABLE: {
    gchar *msg = g_strdup_printf("You are running the latest version of %s.", name);
    if (msg != NULL) {
      GtkWidget *d = gtk_message_dialog_new(w, GTK_DIALOG_MODAL, GTK_MESSAGE_INFO, GTK_BUTTONS_OK, "%s",
                                            msg);
      gtk_lu_apply_dialog_layout(cfg, d);
      gtk_dialog_run(GTK_DIALOG(d));
      gtk_widget_destroy(d);
      g_free(msg);
    }
    break;
  }
  case GTK_LU_BAD_CHECKSUM: {
    GtkWidget *d = gtk_message_dialog_new(
        w, GTK_DIALOG_MODAL, GTK_MESSAGE_ERROR, GTK_BUTTONS_OK,
        "The update information on the server looks invalid (the security "
        "checksum is not in the expected format).\n\n"
        "Nothing was downloaded. Please try again later, or contact the "
        "publisher if the problem continues.");
    gtk_lu_apply_dialog_layout(cfg, d);
    gtk_dialog_run(GTK_DIALOG(d));
    gtk_widget_destroy(d);
    break;
  }
  case UPDATE_AVAILABLE: {
    gchar *body = g_strdup_printf(
        "A newer version of %s is available (version %s).\n\n"
        "Download this update now?",
        name, r->info.version[0] != '\0' ? r->info.version : "(unknown)");
    GtkWidget *d = gtk_message_dialog_new(
        w, GTK_DIALOG_MODAL | GTK_DIALOG_DESTROY_WITH_PARENT, GTK_MESSAGE_QUESTION, GTK_BUTTONS_YES_NO,
        "%s", body != NULL ? body : "Download update?");
    g_free(body);
    gtk_dialog_set_default_response(GTK_DIALOG(d), GTK_RESPONSE_NO);
    gtk_lu_apply_dialog_layout(cfg, d);
    gint response = gtk_dialog_run(GTK_DIALOG(d));
    gtk_widget_destroy(d);

    if (response == GTK_RESPONSE_YES) {
      gtk_lu_start_download(parent, main_widget, cfg, &r->info);
    }
    break;
  }
  case UPDATE_ERROR:
  default: {
    gchar *msg = g_strdup_printf(
        "%s could not read the update information from the server.\n\n"
        "You may be offline, the update service may be busy, or the published "
        "update file may be misconfigured. Try again in a few minutes, or "
        "download the latest version from the publisher's website if you need "
        "the update right away.",
        name);
    if (msg != NULL) {
      GtkWidget *d = gtk_message_dialog_new(w, GTK_DIALOG_MODAL, GTK_MESSAGE_ERROR, GTK_BUTTONS_OK, "%s",
                                            msg);
      gtk_lu_apply_dialog_layout(cfg, d);
      gtk_dialog_run(GTK_DIALOG(d));
      gtk_widget_destroy(d);
      g_free(msg);
    }
    break;
  }
  }

  g_free(r);
  g_free(ud);
}

#endif

void gtk_libupdate_check_for_updates(GtkWindow *parent, const GtkLibupdateConfig *cfg) {
  if (parent == NULL || cfg == NULL) {
    return;
  }

#if !GTK_LIBUPDATE_HAVE_LIBUPDATE
  GtkWidget *d = gtk_message_dialog_new(parent, GTK_DIALOG_MODAL, GTK_MESSAGE_INFO, GTK_BUTTONS_OK,
                                        "This build was not linked with the update library.");
  if (cfg->configure_message_dialog != NULL) {
    cfg->configure_message_dialog(d);
  }
  gtk_dialog_run(GTK_DIALOG(d));
  gtk_widget_destroy(d);
#else
  if (cfg->libupdate_app_name == NULL || cfg->display_name == NULL || cfg->temp_dir_template == NULL) {
    return;
  }

  GtkWidget *main_widget = GTK_WIDGET(parent);

  GtkLuCheckUserData *ud = g_new0(GtkLuCheckUserData, 1);
  ud->parent = parent;
  ud->main_widget = main_widget;
  ud->cfg_copy = *cfg;

  gtk_widget_set_sensitive(main_widget, FALSE);

  GTask *task = g_task_new(G_OBJECT(parent), NULL, gtk_lu_check_complete_fixed, ud);
  g_object_set_data(G_OBJECT(task), "gtk-lu-cfg", (gpointer)&ud->cfg_copy);
  g_task_run_in_thread(task, gtk_lu_check_worker);
  g_object_unref(task);
#endif
}
