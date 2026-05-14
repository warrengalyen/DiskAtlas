#include <gio/gio.h>
#include <gtk/gtk.h>
#ifdef G_OS_WIN32
#  include <windows.h>
#endif

#include "da_fs_monitor.h"
#include "scan_controller.h"

/* ══════════════════════════════════════════════════════════════════════════
 * Windows implementation
 * Uses ReadDirectoryChangesW with bWatchSubtree=TRUE on a background thread
 * so that nested deletions anywhere inside the scan root are detected.
 * GLib's g_file_monitor_directory does not reliably deliver events for
 * drive roots or deep-nested paths on Windows.
 * ══════════════════════════════════════════════════════════════════════════ */
#ifdef G_OS_WIN32

typedef struct {
  AppState *app;
  gchar    *path_utf8;
} DaDeleteIdleData;

static gboolean da_delete_on_main(gpointer user_data) {
  DaDeleteIdleData *d = (DaDeleteIdleData *)user_data;
  if (d->app != NULL && d->app->fs_monitor_pause_for_scan) {
    /* Stale idle from before stop, or posted while a new scan was starting. */
    goto out;
  }
  if (d->app && d->path_utf8 && d->path_utf8[0] != '\0') {
    scan_controller_mark_path_deleted(d->app, d->path_utf8);
  }
out:
  g_free(d->path_utf8);
  g_free(d);
  return G_SOURCE_REMOVE;
}

typedef struct {
  AppState      *app;
  HANDLE         hDir;
  HANDLE         hThread;
  gchar         *root_utf8;   /* original (may use forward slashes) */
  volatile gint  running;     /* g_atomic_int_* access */
} DaWin32Monitor;

static DaWin32Monitor *g_da_win32_mon = NULL;

/* Build an absolute UTF-8 path from the monitor root and a relative
 * path returned by ReadDirectoryChangesW (uses backslashes). */
static gchar *da_win32_build_abs_path(const gchar *root_utf8,
                                      const gunichar2 *fname_w,
                                      glong fname_n_chars) {
  gchar *rel = g_utf16_to_utf8(fname_w, fname_n_chars, NULL, NULL, NULL);
  if (rel == NULL) return NULL;

  /* Normalise relative path separators to match root_utf8 convention.
   * root_utf8 typically uses '/' (GLib / MSYS2 convention); convert '\' → '/'. */
  for (gchar *p = rel; *p; p++) {
    if (*p == '\\') *p = '/';
  }

  /* Ensure root has no trailing separator before g_build_filename so we
   * do not accidentally produce a double separator. */
  gchar *abs;
  gsize rlen = strlen(root_utf8);
  if (rlen > 0 && (root_utf8[rlen - 1] == '/' || root_utf8[rlen - 1] == '\\')) {
    abs = g_strconcat(root_utf8, rel, NULL);
  } else {
    abs = g_strconcat(root_utf8, "/", rel, NULL);
  }
  g_free(rel);
  return abs;
}

static DWORD WINAPI da_win32_monitor_thread(LPVOID param) {
  DaWin32Monitor *mon = (DaWin32Monitor *)param;
  BYTE buf[65536];

  while (g_atomic_int_get(&mon->running)) {
    DWORD returned = 0;
    BOOL ok = ReadDirectoryChangesW(
        mon->hDir, buf, (DWORD)sizeof(buf),
        TRUE,   /* bWatchSubtree: recursive */
        FILE_NOTIFY_CHANGE_FILE_NAME | FILE_NOTIFY_CHANGE_DIR_NAME,
        &returned, NULL, NULL);

    if (!ok || returned == 0) {
      /* Handle was closed (stop request) or an error occurred. */
      break;
    }

    FILE_NOTIFY_INFORMATION *fni = (FILE_NOTIFY_INFORMATION *)(void *)buf;
    for (;;) {
      if (fni->Action == FILE_ACTION_REMOVED ||
          fni->Action == FILE_ACTION_RENAMED_OLD_NAME) {

        glong n = (glong)(fni->FileNameLength / sizeof(WCHAR));
        gchar *abs = da_win32_build_abs_path(mon->root_utf8,
                                             (const gunichar2 *)fni->FileName, n);
        if (abs != NULL) {
          DaDeleteIdleData *d = g_new(DaDeleteIdleData, 1);
          d->app       = mon->app;
          d->path_utf8 = abs;
          /* Must not use g_main_context_invoke_full from this thread: it blocks
           * until the default main context runs the callback. Stopping the
           * monitor (da_win32_monitor_stop_impl) runs on the UI thread and
           * waits on this thread — if we are inside invoke_full, that deadlocks. */
          if (g_idle_add(da_delete_on_main, d) == 0) {
            g_free(abs);
            g_free(d);
          }
        }
      }

      if (fni->NextEntryOffset == 0) break;
      fni = (FILE_NOTIFY_INFORMATION *)((BYTE *)fni + fni->NextEntryOffset);
    }
  }
  return 0;
}

static void da_win32_monitor_stop_impl(void) {
  if (g_da_win32_mon == NULL) return;
  DaWin32Monitor *mon = g_da_win32_mon;
  g_da_win32_mon = NULL;

  g_atomic_int_set(&mon->running, 0);

  /* Tear down directory I/O: cancel any in-flight ReadDirectoryChangesW before CloseHandle.
   * Without CancelIoEx, CloseHandle can block the UI thread indefinitely on some volumes. */
  if (mon->hDir != INVALID_HANDLE_VALUE) {
    (void)CancelIoEx(mon->hDir, NULL);
    CloseHandle(mon->hDir);
    mon->hDir = INVALID_HANDLE_VALUE;
  }
  if (mon->hThread != NULL) {
    WaitForSingleObject(mon->hThread, 3000);
    CloseHandle(mon->hThread);
    mon->hThread = NULL;
  }
  g_free(mon->root_utf8);
  g_free(mon);
}

static void da_win32_monitor_start_impl(AppState *app) {
  da_win32_monitor_stop_impl();

  const gchar *root = app->scan_root_utf8;
  if (root == NULL || root[0] == '\0') return;

  /* Convert the root path to a Windows wide-char path for CreateFileW.
   * Replace forward slashes with backslashes so the Win32 API accepts it. */
  gchar *root_win = g_strdup(root);
  for (gchar *p = root_win; *p; p++) if (*p == '/') *p = '\\';

  gunichar2 *root_w = g_utf8_to_utf16(root_win, -1, NULL, NULL, NULL);
  g_free(root_win);
  if (root_w == NULL) return;

  /* FILE_FLAG_BACKUP_SEMANTICS is required to open a handle to a directory
   * (including volume roots like G:\) with CreateFile. */
  HANDLE hDir = CreateFileW(
      (LPCWSTR)root_w,
      FILE_LIST_DIRECTORY,
      FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
      NULL,
      OPEN_EXISTING,
      FILE_FLAG_BACKUP_SEMANTICS,
      NULL);
  g_free(root_w);

  if (hDir == INVALID_HANDLE_VALUE) {
    g_warning("da_fs_monitor_start: cannot open directory handle for '%s' (Win32 error %lu)",
              root, (unsigned long)GetLastError());
    return;
  }

  DaWin32Monitor *mon = g_new0(DaWin32Monitor, 1);
  mon->app       = app;
  mon->hDir      = hDir;
  mon->root_utf8 = g_strdup(root);  /* keep original forward-slash form */
  g_atomic_int_set(&mon->running, 1);

  mon->hThread = CreateThread(NULL, 0, da_win32_monitor_thread, mon, 0, NULL);
  if (mon->hThread == NULL) {
    CloseHandle(hDir);
    g_free(mon->root_utf8);
    g_free(mon);
    return;
  }
  g_da_win32_mon = mon;
}

#else /* !G_OS_WIN32 ─────────────────────────────────────────────────────── */

/* ══════════════════════════════════════════════════════════════════════════
 * Non-Windows: GLib GFileMonitor (inotify / FSEvents / kqueue)
 * ══════════════════════════════════════════════════════════════════════════ */

static void on_fs_monitor_changed(GFileMonitor      *monitor,
                                  GFile             *file,
                                  GFile             *other_file,
                                  GFileMonitorEvent  event_type,
                                  gpointer           user_data) {
  (void)monitor;
  (void)other_file;
  AppState *app = (AppState *)user_data;
  if (app == NULL) return;
  if (app->fs_monitor_pause_for_scan) {
    return;
  }

  switch (event_type) {
    case G_FILE_MONITOR_EVENT_DELETED:
    case G_FILE_MONITOR_EVENT_MOVED_OUT: {
      gchar *path = g_file_get_path(file);
      if (path != NULL && path[0] != '\0') {
        scan_controller_mark_path_deleted(app, path);
      }
      g_free(path);
      break;
    }

    case G_FILE_MONITOR_EVENT_CREATED:
    case G_FILE_MONITOR_EVENT_CHANGED:
    case G_FILE_MONITOR_EVENT_MOVED_IN:
      if (app->status_label_center != NULL) {
        gtk_label_set_text(GTK_LABEL(app->status_label_center),
                           "File system changed \xe2\x80\x94 re-scan to update");
      }
      break;

    default:
      break;
  }
}

#endif /* G_OS_WIN32 */

/* ── public API ─────────────────────────────────────────────────────────── */

void da_fs_monitor_start(AppState *app) {
  if (app == NULL || !app->general_fs_monitor) return;
  if (app->fs_monitor_pause_for_scan) {
    return;
  }
  if (app->scan_root_utf8 == NULL || app->scan_root_utf8[0] == '\0') return;

#ifdef G_OS_WIN32
  da_win32_monitor_start_impl(app);
#else
  da_fs_monitor_stop(app);

  GError *err = NULL;
  GFile  *f   = g_file_new_for_path(app->scan_root_utf8);
  app->fs_monitor = g_file_monitor_directory(f, G_FILE_MONITOR_WATCH_MOVES, NULL, &err);
  g_object_unref(f);

  if (app->fs_monitor == NULL) {
    if (err != NULL) {
      g_warning("da_fs_monitor_start: %s", err->message);
      g_clear_error(&err);
    }
    return;
  }

  g_signal_connect(app->fs_monitor, "changed", G_CALLBACK(on_fs_monitor_changed), app);
#endif
}

void da_fs_monitor_scan_phase_begin(AppState *app) {
  if (app == NULL) {
    return;
  }
  da_fs_monitor_stop(app);
  app->fs_monitor_pause_for_scan = TRUE;
}

void da_fs_monitor_scan_phase_end(AppState *app) {
  if (app == NULL) {
    return;
  }
  app->fs_monitor_pause_for_scan = FALSE;
  da_fs_monitor_start(app);
}

void da_fs_monitor_stop(AppState *app) {
#ifdef G_OS_WIN32
  (void)app;
  da_win32_monitor_stop_impl();
#else
  if (app == NULL || app->fs_monitor == NULL) return;
  g_file_monitor_cancel(app->fs_monitor);
  g_object_unref(app->fs_monitor);
  app->fs_monitor = NULL;
#endif
}
