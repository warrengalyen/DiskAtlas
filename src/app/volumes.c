#include <stddef.h>
#include <stdio.h>
#include <string.h>

#include "volumes.h"

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <gtk/gtk.h>

static int utf8_to_wide(const char *utf8, WCHAR *out, int cchOut) {
  if (!utf8 || !out || cchOut <= 0) {
    return 0;
  }
  int n = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, utf8, -1, out, cchOut);
  if (n <= 0) {
    n = MultiByteToWideChar(CP_UTF8, 0, utf8, -1, out, cchOut);
  }
  return n > 0 ? n : 0;
}

int da_volume_space_for_path(const char *path_utf8, uint64_t *total, uint64_t *free_bytes,
                             uint64_t *used_bytes) {
  WCHAR wpath[4096];
  if (!utf8_to_wide(path_utf8, wpath, (int)(sizeof(wpath) / sizeof(wpath[0])))) {
    return -1;
  }
  WCHAR root[8];
  if (wpath[0] != L'\0' && wpath[1] == L':') {
    root[0] = wpath[0];
    root[1] = L':';
    root[2] = L'\\';
    root[3] = L'\0';
  } else {
    root[0] = L'\0';
  }
  ULARGE_INTEGER free_caller = {0}, total_all = {0}, total_free = {0};
  if (!GetDiskFreeSpaceExW(root[0] ? root : wpath, &free_caller, &total_all, &total_free)) {
    return -1;
  }
  if (total) {
    *total = total_all.QuadPart;
  }
  if (free_bytes) {
    *free_bytes = total_free.QuadPart;
  }
  if (used_bytes) {
    if (total_all.QuadPart >= total_free.QuadPart) {
      *used_bytes = total_all.QuadPart - total_free.QuadPart;
    } else {
      *used_bytes = 0;
    }
  }
  return 0;
}

void da_volume_selection_label(const char *path_utf8, char *out, size_t out_sz) {
  if (out == NULL || out_sz == 0) {
    return;
  }
  out[0] = '\0';
  if (path_utf8 == NULL || path_utf8[0] == '\0') {
    return;
  }
  WCHAR wpath[4096];
  if (!utf8_to_wide(path_utf8, wpath, (int)(sizeof(wpath) / sizeof(wpath[0])))) {
    snprintf(out, out_sz, "%s", path_utf8);
    return;
  }
  if (wpath[0] == L'\\' && wpath[1] == L'\\') {
    snprintf(out, out_sz, "%s", path_utf8);
    return;
  }
  WCHAR d = wpath[0];
  if (!((d >= L'A' && d <= L'Z') || (d >= L'a' && d <= L'z')) || wpath[1] != L':') {
    snprintf(out, out_sz, "%s", path_utf8);
    return;
  }
  if (d >= L'a' && d <= L'z') {
    d = (WCHAR)(d - L'a' + L'A');
  }
  WCHAR root[4] = {d, L':', L'\\', L'\0'};
  WCHAR vol[MAX_PATH + 1];
  if (!GetVolumeInformationW(root, vol, MAX_PATH + 1, NULL, NULL, NULL, NULL, 0)) {
    snprintf(out, out_sz, "[%c:]", (char)d);
    return;
  }
  char letter = (char)d;
  char vol_utf8[512];
  vol_utf8[0] = '\0';
  if (vol[0] != L'\0') {
    if (WideCharToMultiByte(CP_UTF8, 0, vol, -1, vol_utf8, (int)sizeof(vol_utf8), NULL, NULL) <= 0) {
      vol_utf8[0] = '\0';
    }
  }
  if (vol_utf8[0] != '\0') {
    snprintf(out, out_sz, "[%c:] %s", letter, vol_utf8);
  } else {
    snprintf(out, out_sz, "[%c:]", letter);
  }
}

static gint cmp_utf8_drive_path(gconstpointer ap, gconstpointer bp) {
  const gchar *pa = *(const gchar *const *)ap;
  const gchar *pb = *(const gchar *const *)bp;
  return strcmp(pa, pb);
}

void da_win32_file_chooser_set_drive_places_only(GtkFileChooser *chooser) {
  GSList *uris = gtk_file_chooser_list_shortcut_folder_uris(chooser);
  for (GSList *l = uris; l != NULL; l = l->next) {
    GError *err = NULL;
    (void)gtk_file_chooser_remove_shortcut_folder_uri(chooser, (const gchar *)l->data, &err);
    g_clear_error(&err);
  }
  g_slist_free_full(uris, g_free);

  WCHAR buf[512];
  DWORD nw = GetLogicalDriveStringsW((DWORD)(sizeof(buf) / sizeof(buf[0])), buf);
  if (nw == 0 || nw >= sizeof(buf) / sizeof(buf[0])) {
    return;
  }

  GPtrArray *paths = g_ptr_array_new_with_free_func(g_free);
  const WCHAR *p = buf;
  while (*p != L'\0') {
    int nch = lstrlenW(p);
    if (nch > 0) {
      gchar *utf8 = g_utf16_to_utf8(p, -1, NULL, NULL, NULL);
      if (utf8 != NULL) {
        gchar *with_sep = utf8;
        if (!g_str_has_suffix(utf8, G_DIR_SEPARATOR_S)) {
          with_sep = g_strconcat(utf8, G_DIR_SEPARATOR_S, NULL);
          g_free(utf8);
        }
        g_ptr_array_add(paths, with_sep);
      }
    }
    p += nch + 1;
  }

  g_ptr_array_sort(paths, cmp_utf8_drive_path);

  for (guint i = 0; i < paths->len; i++) {
    const gchar *local_path = g_ptr_array_index(paths, i);
    GError *conv_err = NULL;
    gchar *uri = g_filename_to_uri(local_path, NULL, &conv_err);
    g_clear_error(&conv_err);
    if (uri == NULL) {
      continue;
    }
    GError *add_err = NULL;
    (void)gtk_file_chooser_add_shortcut_folder_uri(chooser, uri, &add_err);
    g_clear_error(&add_err);
    g_free(uri);
  }
  g_ptr_array_unref(paths);
}

#else /* !_WIN32 */

#include <errno.h>
#include <sys/statvfs.h>

int da_volume_space_for_path(const char *path_utf8, uint64_t *total, uint64_t *free_bytes,
                             uint64_t *used_bytes) {
  struct statvfs vfs;
  memset(&vfs, 0, sizeof(vfs));
  if (statvfs(path_utf8, &vfs) != 0) {
    return -1;
  }
  uint64_t fr = (uint64_t)vfs.f_frsize;
  uint64_t blocks = (uint64_t)vfs.f_blocks;
  uint64_t bavail = (uint64_t)vfs.f_bavail;
  uint64_t bfree = (uint64_t)vfs.f_bfree;
  uint64_t t = fr * blocks;
  uint64_t f = fr * bavail;
  if (total) {
    *total = t;
  }
  if (free_bytes) {
    *free_bytes = f;
  }
  if (used_bytes) {
    *used_bytes = (t > f) ? (t - f) : 0;
  }
  (void)bfree;
  return 0;
}

void da_volume_selection_label(const char *path_utf8, char *out, size_t out_sz) {
  if (out == NULL || out_sz == 0) {
    return;
  }
  if (path_utf8 == NULL || path_utf8[0] == '\0') {
    out[0] = '\0';
    return;
  }
  snprintf(out, out_sz, "%s", path_utf8);
}

#endif /* _WIN32 */
