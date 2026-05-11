#include <stddef.h>
#include <string.h>

#include <glib.h>
#include <gio/gio.h>
#include <gtk/gtk.h>

#include "file_ops.h"

/* =========================================================================
 * Windows implementation
 * ========================================================================= */

#if defined(_WIN32)

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <shellapi.h>
#include <shlobj.h>

/* Convert a NULL-terminated UTF-8 string to a newly allocated wide string.
 * Returns NULL on failure.  Caller must g_free() the result. */
static WCHAR *utf8_to_wchar_alloc(const char *utf8) {
  if (utf8 == NULL) {
    return NULL;
  }
  int need = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, utf8, -1, NULL, 0);
  if (need <= 0) {
    need = MultiByteToWideChar(CP_UTF8, 0, utf8, -1, NULL, 0);
  }
  if (need <= 0) {
    return NULL;
  }
  WCHAR *out = (WCHAR *)g_malloc((size_t)need * sizeof(WCHAR));
  if (out == NULL) {
    return NULL;
  }
  int n = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, utf8, -1, out, need);
  if (n <= 0) {
    n = MultiByteToWideChar(CP_UTF8, 0, utf8, -1, out, need);
  }
  if (n <= 0) {
    g_free(out);
    return NULL;
  }
  return out;
}

/* Build a double-null-terminated wide string with all paths concatenated.
 * Each path is separated by a single NUL, with two NULs at the very end.
 * Returns a HGLOBAL for use with SetClipboardData / SHFileOperationW. */
static HGLOBAL build_double_null_wpath_list(const char **paths, size_t count, size_t *out_total_wchars) {
  if (out_total_wchars) {
    *out_total_wchars = 0;
  }
  if (paths == NULL || count == 0) {
    return NULL;
  }

  /* First pass: measure total wide-char count (each path including its NUL). */
  size_t total = 0;
  for (size_t i = 0; i < count; i++) {
    if (paths[i] == NULL || paths[i][0] == '\0') {
      continue;
    }
    int n = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, paths[i], -1, NULL, 0);
    if (n <= 0) {
      n = MultiByteToWideChar(CP_UTF8, 0, paths[i], -1, NULL, 0);
    }
    if (n > 0) {
      total += (size_t)n; /* includes the trailing NUL of each path */
    }
  }
  total += 1; /* extra NUL for double-null termination */

  HGLOBAL hg = GlobalAlloc(GHND, total * sizeof(WCHAR));
  if (hg == NULL) {
    return NULL;
  }
  WCHAR *buf = (WCHAR *)GlobalLock(hg);
  if (buf == NULL) {
    GlobalFree(hg);
    return NULL;
  }

  WCHAR *p = buf;
  for (size_t i = 0; i < count; i++) {
    if (paths[i] == NULL || paths[i][0] == '\0') {
      continue;
    }
    int n = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, paths[i], -1, p, (int)(total - (size_t)(p - buf)));
    if (n <= 0) {
      n = MultiByteToWideChar(CP_UTF8, 0, paths[i], -1, p, (int)(total - (size_t)(p - buf)));
    }
    if (n > 0) {
      p += n; /* p now points past the NUL of this path */
    }
  }
  /* Ensure double-null termination (GlobalAlloc GHND zeroes memory, but be explicit). */
  if (p < buf + total) {
    *p = L'\0';
  }

  GlobalUnlock(hg);
  if (out_total_wchars) {
    *out_total_wchars = total;
  }
  return hg;
}

/* Put CF_HDROP on the clipboard with the given paths.
 * is_cut: also set "Preferred DropEffect" = DROPEFFECT_MOVE. */
static gboolean win32_clipboard_set_paths(const char **paths, size_t count, gboolean is_cut) {
  if (paths == NULL || count == 0) {
    return FALSE;
  }

  /* Build the path list. */
  size_t total_wchars = 0;
  HGLOBAL hpath = build_double_null_wpath_list(paths, count, &total_wchars);
  if (hpath == NULL) {
    return FALSE;
  }

  /* Build the DROPFILES structure + path list in a single HGLOBAL. */
  size_t df_size = sizeof(DROPFILES) + total_wchars * sizeof(WCHAR);
  HGLOBAL hdrop = GlobalAlloc(GHND, df_size);
  if (hdrop == NULL) {
    GlobalFree(hpath);
    return FALSE;
  }
  DROPFILES *df = (DROPFILES *)GlobalLock(hdrop);
  if (df == NULL) {
    GlobalFree(hpath);
    GlobalFree(hdrop);
    return FALSE;
  }
  df->pFiles = sizeof(DROPFILES);
  df->pt.x   = 0;
  df->pt.y   = 0;
  df->fNC    = FALSE;
  df->fWide  = TRUE;

  /* Copy path list into the same block, right after the DROPFILES header. */
  WCHAR *path_buf = (WCHAR *)GlobalLock(hpath);
  if (path_buf == NULL) {
    GlobalUnlock(hdrop);
    GlobalFree(hpath);
    GlobalFree(hdrop);
    return FALSE;
  }
  memcpy((char *)df + sizeof(DROPFILES), path_buf, total_wchars * sizeof(WCHAR));
  GlobalUnlock(hpath);
  GlobalFree(hpath);
  GlobalUnlock(hdrop);

  if (!OpenClipboard(NULL)) {
    GlobalFree(hdrop);
    return FALSE;
  }
  EmptyClipboard();
  SetClipboardData(CF_HDROP, hdrop);

  if (is_cut) {
    /* Signal cut to Explorer / other shell consumers. */
    static UINT cf_preferred_effect = 0;
    if (cf_preferred_effect == 0) {
      cf_preferred_effect = RegisterClipboardFormatW(L"Preferred DropEffect");
    }
    if (cf_preferred_effect != 0) {
      /* DROPEFFECT_MOVE = 2 */
      HGLOBAL heff = GlobalAlloc(GHND, sizeof(DWORD));
      if (heff != NULL) {
        DWORD *eff = (DWORD *)GlobalLock(heff);
        if (eff != NULL) {
          *eff = 2; /* DROPEFFECT_MOVE */
          GlobalUnlock(heff);
        }
        SetClipboardData(cf_preferred_effect, heff);
      }
    }
  }

  CloseClipboard();
  return TRUE;
}

gboolean da_file_op_copy_to_clipboard(GtkWidget *window, const char **paths, size_t count) {
  (void)window;
  return win32_clipboard_set_paths(paths, count, FALSE);
}

gboolean da_file_op_cut_to_clipboard(GtkWidget *window, const char **paths, size_t count) {
  (void)window;
  return win32_clipboard_set_paths(paths, count, TRUE);
}

/* Perform a SHFileOperationW on the given paths with the specified flags. */
static gboolean win32_shfileop(const char **paths, size_t count, UINT wFunc, FILEOP_FLAGS flags) {
  if (paths == NULL || count == 0) {
    return FALSE;
  }
  size_t total_wchars = 0;
  HGLOBAL hg = build_double_null_wpath_list(paths, count, &total_wchars);
  if (hg == NULL) {
    return FALSE;
  }
  WCHAR *wpath_list = (WCHAR *)GlobalLock(hg);
  if (wpath_list == NULL) {
    GlobalFree(hg);
    return FALSE;
  }

  SHFILEOPSTRUCTW op;
  memset(&op, 0, sizeof(op));
  op.wFunc  = wFunc;
  op.pFrom  = wpath_list;
  op.fFlags = flags;

  int ret = SHFileOperationW(&op);
  GlobalUnlock(hg);
  GlobalFree(hg);
  return (ret == 0 && !op.fAnyOperationsAborted);
}

gboolean da_file_op_trash(const char **paths, size_t count, GError **error) {
  /* FOF_ALLOWUNDO sends to Recycle Bin; FOF_NOCONFIRMATION suppresses per-file prompts. */
  gboolean ok = win32_shfileop(paths, count, FO_DELETE,
    FOF_ALLOWUNDO | FOF_NOCONFIRMATION | FOF_NOERRORUI | FOF_SILENT);
  if (!ok && error != NULL) {
    *error = g_error_new(G_IO_ERROR, G_IO_ERROR_FAILED,
                         "Move to Recycle Bin failed or was aborted.");
  }
  return ok;
}

gboolean da_file_op_delete_permanent(const char **paths, size_t count, GError **error) {
  gboolean ok = win32_shfileop(paths, count, FO_DELETE,
    FOF_NOCONFIRMATION | FOF_NOERRORUI | FOF_SILENT);
  if (!ok && error != NULL) {
    *error = g_error_new(G_IO_ERROR, G_IO_ERROR_FAILED,
                         "Permanent delete failed or was aborted.");
  }
  return ok;
}

/* =========================================================================
 * POSIX (Linux / macOS) implementation
 * ========================================================================= */

#else /* !_WIN32 */

/* ---- Clipboard helpers ---- */

typedef struct {
  gchar *gnome_text;    /* "copy\nfile://...\n..." or "cut\n..." */
  gchar *uri_list;      /* "file://...\r\n..." */
} ClipboardPayload;

static void clipboard_payload_free(ClipboardPayload *p) {
  if (p == NULL) {
    return;
  }
  g_free(p->gnome_text);
  g_free(p->uri_list);
  g_free(p);
}

static void clipboard_get_func(GtkClipboard *clipboard, GtkSelectionData *sel,
                                guint info, gpointer user_data) {
  (void)clipboard;
  ClipboardPayload *p = (ClipboardPayload *)user_data;
  if (p == NULL) {
    return;
  }
  if (info == 0) {
    /* x-special/gnome-copied-files */
    gtk_selection_data_set(sel,
      gtk_selection_data_get_target(sel),
      8 /* bits per element */,
      (const guchar *)p->gnome_text,
      p->gnome_text ? (gint)strlen(p->gnome_text) : 0);
  } else {
    /* text/uri-list */
    gtk_selection_data_set(sel,
      gtk_selection_data_get_target(sel),
      8,
      (const guchar *)p->uri_list,
      p->uri_list ? (gint)strlen(p->uri_list) : 0);
  }
}

static void clipboard_clear_func(GtkClipboard *clipboard, gpointer user_data) {
  (void)clipboard;
  clipboard_payload_free((ClipboardPayload *)user_data);
}

/* Build file:// URI from a local UTF-8 path.
 * Returns a newly allocated string that must be g_free()'d. */
static gchar *path_to_file_uri(const char *path) {
  GFile *f = g_file_new_for_path(path);
  gchar *uri = g_file_get_uri(f);
  g_object_unref(f);
  return uri;
}

static gboolean posix_clipboard_set_paths(GtkWidget *window, const char **paths, size_t count,
                                           gboolean is_cut) {
  if (paths == NULL || count == 0 || window == NULL) {
    return FALSE;
  }

  GString *gnome = g_string_new(is_cut ? "cut\n" : "copy\n");
  GString *urilist = g_string_new(NULL);

  for (size_t i = 0; i < count; i++) {
    if (paths[i] == NULL || paths[i][0] == '\0') {
      continue;
    }
    gchar *uri = path_to_file_uri(paths[i]);
    if (uri == NULL) {
      continue;
    }
    g_string_append(gnome, uri);
    g_string_append_c(gnome, '\n');
    g_string_append(urilist, uri);
    g_string_append(urilist, "\r\n");
    g_free(uri);
  }

  ClipboardPayload *payload = g_new0(ClipboardPayload, 1);
  payload->gnome_text = g_string_free(gnome, FALSE);
  payload->uri_list   = g_string_free(urilist, FALSE);

  GtkTargetEntry targets[] = {
    { (gchar *)"x-special/gnome-copied-files", 0, 0 },
    { (gchar *)"text/uri-list",                0, 1 },
  };

  GtkClipboard *cb = gtk_widget_get_clipboard(window, GDK_SELECTION_CLIPBOARD);
  gboolean ok = gtk_clipboard_set_with_data(cb, targets, G_N_ELEMENTS(targets),
                                             clipboard_get_func,
                                             clipboard_clear_func,
                                             payload);
  if (!ok) {
    clipboard_payload_free(payload);
  }
  return ok;
}

gboolean da_file_op_copy_to_clipboard(GtkWidget *window, const char **paths, size_t count) {
  return posix_clipboard_set_paths(window, paths, count, FALSE);
}

gboolean da_file_op_cut_to_clipboard(GtkWidget *window, const char **paths, size_t count) {
  return posix_clipboard_set_paths(window, paths, count, TRUE);
}

/* ---- Trash / delete helpers ---- */

gboolean da_file_op_trash(const char **paths, size_t count, GError **error) {
  if (paths == NULL || count == 0) {
    return TRUE;
  }
  gboolean all_ok = TRUE;
  for (size_t i = 0; i < count; i++) {
    if (paths[i] == NULL || paths[i][0] == '\0') {
      continue;
    }
    GFile *f = g_file_new_for_path(paths[i]);
    GError *local_err = NULL;
    gboolean ok = g_file_trash(f, NULL, &local_err);
    g_object_unref(f);
    if (!ok) {
      if (all_ok && error != NULL && *error == NULL) {
        g_propagate_error(error, local_err);
      } else {
        g_clear_error(&local_err);
      }
      all_ok = FALSE;
    }
  }
  return all_ok;
}

/* Recursively delete a GFile tree using GIO. */
static gboolean delete_recursive(GFile *file, GCancellable *cancel, GError **error) {
  GFileInfo *info = g_file_query_info(file,
    G_FILE_ATTRIBUTE_STANDARD_TYPE,
    G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS,
    cancel, NULL);
  GFileType type = G_FILE_TYPE_UNKNOWN;
  if (info != NULL) {
    type = g_file_info_get_file_type(info);
    g_object_unref(info);
  }

  if (type == G_FILE_TYPE_DIRECTORY) {
    GError *enum_err = NULL;
    GFileEnumerator *en = g_file_enumerate_children(file,
      G_FILE_ATTRIBUTE_STANDARD_NAME,
      G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS,
      cancel, &enum_err);
    if (en == NULL) {
      g_propagate_error(error, enum_err);
      return FALSE;
    }

    gboolean dir_ok = TRUE;
    GFileInfo *child_info = NULL;
    GError *child_err = NULL;
    while ((child_info = g_file_enumerator_next_file(en, cancel, &child_err)) != NULL) {
      const gchar *name = g_file_info_get_name(child_info);
      GFile *child = g_file_get_child(file, name);
      GError *rec_err = NULL;
      if (!delete_recursive(child, cancel, &rec_err)) {
        if (dir_ok && error != NULL && *error == NULL) {
          g_propagate_error(error, rec_err);
        } else {
          g_clear_error(&rec_err);
        }
        dir_ok = FALSE;
      }
      g_object_unref(child);
      g_object_unref(child_info);
    }
    if (child_err != NULL) {
      if (dir_ok && error != NULL && *error == NULL) {
        g_propagate_error(error, child_err);
      } else {
        g_clear_error(&child_err);
      }
      dir_ok = FALSE;
    }
    g_object_unref(en);
    if (!dir_ok) {
      return FALSE;
    }
  }

  /* Delete the (now-empty) directory or the file itself. */
  GError *del_err = NULL;
  gboolean ok = g_file_delete(file, cancel, &del_err);
  if (!ok) {
    if (error != NULL && *error == NULL) {
      g_propagate_error(error, del_err);
    } else {
      g_clear_error(&del_err);
    }
  }
  return ok;
}

gboolean da_file_op_delete_permanent(const char **paths, size_t count, GError **error) {
  if (paths == NULL || count == 0) {
    return TRUE;
  }
  gboolean all_ok = TRUE;
  for (size_t i = 0; i < count; i++) {
    if (paths[i] == NULL || paths[i][0] == '\0') {
      continue;
    }
    GFile *f = g_file_new_for_path(paths[i]);
    GError *local_err = NULL;
    gboolean ok = delete_recursive(f, NULL, &local_err);
    g_object_unref(f);
    if (!ok) {
      if (all_ok && error != NULL && *error == NULL) {
        g_propagate_error(error, local_err);
      } else {
        g_clear_error(&local_err);
      }
      all_ok = FALSE;
    }
  }
  return all_ok;
}

#endif /* _WIN32 */
