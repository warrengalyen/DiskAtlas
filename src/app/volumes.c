#include <stddef.h>
#include <stdio.h>
#include <string.h>

#include "volumes.h"

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <shellapi.h>

#include <glib.h>
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
  snprintf(out, out_sz, "%s", path_utf8);
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

gboolean da_win32_is_process_elevated(void) {
  HANDLE tok = NULL;
  if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &tok)) {
    return FALSE;
  }
  TOKEN_ELEVATION el;
  DWORD w = 0;
  BOOL ok = GetTokenInformation(tok, TokenElevation, &el, sizeof(el), &w);
  CloseHandle(tok);
  return ok && el.TokenIsElevated;
}

static gchar *da_win32_settings_path(void) {
  return g_build_filename(g_get_user_config_dir(), "diskatlas", "settings.ini", NULL);
}

gboolean da_win32_admin_ntfs_notice_saved_hidden(void) {
  gchar *path = da_win32_settings_path();
  GKeyFile *kf = g_key_file_new();
  gboolean hide = FALSE;
  if (g_key_file_load_from_file(kf, path, G_KEY_FILE_NONE, NULL)) {
    if (g_key_file_has_key(kf, "ui", "hide_admin_ntfs_notice", NULL)) {
      hide = g_key_file_get_boolean(kf, "ui", "hide_admin_ntfs_notice", NULL);
    }
  }
  g_key_file_unref(kf);
  g_free(path);
  return hide;
}

void da_win32_set_admin_ntfs_notice_hidden(gboolean hide) {
  gchar *path = da_win32_settings_path();
  gchar *dir = g_path_get_dirname(path);
  g_mkdir_with_parents(dir, 0700);
  g_free(dir);

  GKeyFile *kf = g_key_file_new();
  (void)g_key_file_load_from_file(kf, path, G_KEY_FILE_NONE, NULL);
  g_key_file_set_boolean(kf, "ui", "hide_admin_ntfs_notice", hide);
  gsize len = 0;
  gchar *data = g_key_file_to_data(kf, &len, NULL);
  g_key_file_unref(kf);
  if (data != NULL) {
    GError *werr = NULL;
    g_file_set_contents(path, data, (gssize)len, &werr);
    g_clear_error(&werr);
    g_free(data);
  }
  g_free(path);
}

static LPWSTR skip_first_command_line_arg(LPWSTR cmd) {
  if (cmd == NULL || *cmd == L'\0') {
    return cmd;
  }
  LPWSTR p = cmd;
  if (*p == L'"') {
    p++;
    while (*p != L'\0' && *p != L'"') {
      p++;
    }
    if (*p == L'"') {
      p++;
    }
  } else {
    while (*p != L'\0' && *p != L' ' && *p != L'\t') {
      p++;
    }
  }
  while (*p == L' ' || *p == L'\t') {
    p++;
  }
  return p;
}

gboolean da_win32_restart_elevated_self(void) {
  WCHAR exe[MAX_PATH];
  DWORD n = GetModuleFileNameW(NULL, exe, MAX_PATH);
  if (n == 0 || n >= MAX_PATH) {
    return FALSE;
  }
  LPWSTR tail = skip_first_command_line_arg(GetCommandLineW());
  HINSTANCE hi =
      ShellExecuteW(NULL, L"runas", exe, (tail != NULL && tail[0] != L'\0') ? tail : NULL, NULL, SW_SHOW);
  return (INT_PTR)hi > 32;
}

static gchar *win32_volume_display_label(WCHAR drive_letter) {
  WCHAR root[4] = {drive_letter, L':', L'\\', L'\0'};
  WCHAR vol[MAX_PATH + 1];
  WCHAR d = drive_letter;
  if (d >= L'a' && d <= L'z') {
    d = (WCHAR)(d - L'a' + L'A');
  }
  char letter = (char)d;
  memset(vol, 0, sizeof(vol));
  if (!GetVolumeInformationW(root, vol, MAX_PATH + 1, NULL, NULL, NULL, NULL, 0)) {
    return g_strdup_printf("[%c:]", letter);
  }
  char vol_utf8[512];
  vol_utf8[0] = '\0';
  if (vol[0] != L'\0') {
    if (WideCharToMultiByte(CP_UTF8, 0, vol, -1, vol_utf8, (int)sizeof(vol_utf8), NULL, NULL) <= 0) {
      vol_utf8[0] = '\0';
    }
  }
  if (vol_utf8[0] != '\0') {
    return g_strdup_printf("[%c:] %s", letter, vol_utf8);
  }
  return g_strdup_printf("[%c:]", letter);
}

static gint cmp_volume_entry_ptr(gconstpointer a, gconstpointer b) {
  const DaVolumeEntry *ea = *(const DaVolumeEntry *const *)a;
  const DaVolumeEntry *eb = *(const DaVolumeEntry *const *)b;
  return g_utf8_collate(ea->root_path, eb->root_path);
}

int da_volume_enumerate(DaVolumeEntry **entries, gsize *n_out) {
  if (entries == NULL || n_out == NULL) {
    return -1;
  }
  *entries = NULL;
  *n_out = 0;

  WCHAR buf[512];
  DWORD nw = GetLogicalDriveStringsW((DWORD)(sizeof(buf) / sizeof(buf[0])), buf);
  if (nw == 0 || nw >= sizeof(buf) / sizeof(WCHAR)) {
    return -1;
  }

  GPtrArray *pa = g_ptr_array_new();
  const WCHAR *p = buf;
  while (*p != L'\0') {
    int nch = lstrlenW(p);
    if (nch > 0) {
      WCHAR dl = p[0];
      if (((dl >= L'A' && dl <= L'Z') || (dl >= L'a' && dl <= L'z')) && nch >= 2 && p[1] == L':') {
        gchar *utf8 = g_utf16_to_utf8(p, -1, NULL, NULL, NULL);
        if (utf8 != NULL) {
          gchar *with_sep = utf8;
          if (!g_str_has_suffix(utf8, G_DIR_SEPARATOR_S)) {
            with_sep = g_strconcat(utf8, G_DIR_SEPARATOR_S, NULL);
            g_free(utf8);
          }
          gchar *lab = win32_volume_display_label(dl);
          DaVolumeEntry *e = g_new0(DaVolumeEntry, 1);
          e->root_path = with_sep;
          e->display_label = lab;
          g_ptr_array_add(pa, e);
        }
      }
    }
    p += nch + 1;
  }

  g_ptr_array_sort(pa, cmp_volume_entry_ptr);

  gsize n = pa->len;
  DaVolumeEntry *arr = g_new0(DaVolumeEntry, n);
  for (gsize i = 0; i < n; i++) {
    DaVolumeEntry *src = g_ptr_array_index(pa, i);
    arr[i].root_path = src->root_path;
    arr[i].display_label = src->display_label;
    g_free(src);
  }
  g_ptr_array_free(pa, TRUE);

  *entries = arr;
  *n_out = n;
  return 0;
}

#else /* !_WIN32 */

#include <errno.h>
#include <stdlib.h>
#include <sys/statvfs.h>

#include <gio/gio.h>

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

static gint cmp_volume_entry_ptr_posix(gconstpointer a, gconstpointer b) {
  const DaVolumeEntry *ea = *(const DaVolumeEntry *const *)a;
  const DaVolumeEntry *eb = *(const DaVolumeEntry *const *)b;
  return g_utf8_collate(ea->root_path, eb->root_path);
}

int da_volume_enumerate(DaVolumeEntry **entries, gsize *n_out) {
  if (entries == NULL || n_out == NULL) {
    return -1;
  }
  *entries = NULL;
  *n_out = 0;

  GPtrArray *pa = g_ptr_array_new();
  GHashTable *seen =
      g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);

  GVolumeMonitor *vm = g_volume_monitor_get();
  GList *mounts = g_volume_monitor_get_mounts(vm);
  for (GList *l = mounts; l != NULL; l = l->next) {
    GMount *m = G_MOUNT(l->data);
    GFile *root = g_mount_get_root(m);
    gchar *path = root != NULL ? g_file_get_path(root) : NULL;
    if (root != NULL) {
      g_object_unref(root);
    }
    if (path == NULL || g_hash_table_contains(seen, path)) {
      g_free(path);
      continue;
    }

    gchar *mname = g_mount_get_name(m);
    gchar *label = g_strdup_printf("[%s] %s", path, mname != NULL ? mname : "");
    g_free(mname);

    DaVolumeEntry *e = g_new0(DaVolumeEntry, 1);
    e->root_path = path;
    e->display_label = label;
    g_hash_table_insert(seen, g_strdup(path), GINT_TO_POINTER(1));
    g_ptr_array_add(pa, e);
  }
  g_list_free(mounts);
  g_hash_table_destroy(seen);

  gboolean have_root = FALSE;
  for (guint i = 0; i < pa->len; i++) {
    DaVolumeEntry *e = g_ptr_array_index(pa, i);
    if (g_strcmp0(e->root_path, "/") == 0) {
      have_root = TRUE;
      break;
    }
  }
  if (!have_root) {
    DaVolumeEntry *e = g_new0(DaVolumeEntry, 1);
    e->root_path = g_strdup("/");
    e->display_label = g_strdup("[/] /");
    g_ptr_array_add(pa, e);
  }

  g_ptr_array_sort(pa, cmp_volume_entry_ptr_posix);

  gsize n = pa->len;
  DaVolumeEntry *arr = g_new0(DaVolumeEntry, n);
  for (gsize i = 0; i < n; i++) {
    DaVolumeEntry *src = g_ptr_array_index(pa, i);
    arr[i].root_path = src->root_path;
    arr[i].display_label = src->display_label;
    g_free(src);
  }
  g_ptr_array_free(pa, TRUE);

  *entries = arr;
  *n_out = n;
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

void da_volume_list_free(DaVolumeEntry *entries, gsize n) {
  if (entries == NULL) {
    return;
  }
  for (gsize i = 0; i < n; i++) {
    g_free(entries[i].root_path);
    g_free(entries[i].display_label);
  }
  g_free(entries);
}

gchar *da_volume_system_root_utf8(void) {
#if defined(_WIN32)
  const gchar *sd = g_getenv("SystemDrive");
  if (sd != NULL && sd[0] != '\0' && g_ascii_isalpha((guchar)sd[0])) {
    char up = (char)g_ascii_toupper((guchar)sd[0]);
    return g_strdup_printf("%c:\\", up);
  }
  return g_strdup("C:\\");
#else
  return g_strdup("/");
#endif
}

gboolean da_volume_is_exact_root_path(const gchar *path_utf8, const gchar *volume_root_utf8) {
  if (path_utf8 == NULL || volume_root_utf8 == NULL) {
    return FALSE;
  }
#if defined(_WIN32)
  gchar *norm_path = g_strdup(path_utf8);
  gchar *norm_root = g_strdup(volume_root_utf8);
  for (gchar *s = norm_path; *s != '\0'; s++) {
    if (*s == '/') {
      *s = '\\';
    }
  }
  for (gchar *s = norm_root; *s != '\0'; s++) {
    if (*s == '/') {
      *s = '\\';
    }
  }
  if (strlen(norm_path) >= 2 && g_ascii_isalpha((guchar)norm_path[0]) && norm_path[1] == ':') {
    norm_path[0] = (gchar)g_ascii_toupper((guchar)norm_path[0]);
  }
  if (strlen(norm_root) >= 2 && g_ascii_isalpha((guchar)norm_root[0]) && norm_root[1] == ':') {
    norm_root[0] = (gchar)g_ascii_toupper((guchar)norm_root[0]);
  }
  size_t lp = strlen(norm_path);
  size_t lr = strlen(norm_root);
  if (lp >= 2 && norm_path[lp - 1] != '\\') {
    gchar *t = g_strconcat(norm_path, "\\", NULL);
    g_free(norm_path);
    norm_path = t;
    lp = strlen(norm_path);
  }
  if (lr >= 2 && norm_root[lr - 1] != '\\') {
    gchar *t = g_strconcat(norm_root, "\\", NULL);
    g_free(norm_root);
    norm_root = t;
    lr = strlen(norm_root);
  }
  gboolean eq = (g_strcmp0(norm_path, norm_root) == 0);
  g_free(norm_path);
  g_free(norm_root);
  return eq;
#else
  gchar *ca = g_canonicalize_filename(path_utf8, NULL);
  gchar *cb = g_canonicalize_filename(volume_root_utf8, NULL);
  gboolean eq = (g_strcmp0(ca, cb) == 0);
  g_free(ca);
  g_free(cb);
  return eq;
#endif
}
