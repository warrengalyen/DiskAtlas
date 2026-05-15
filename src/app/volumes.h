#ifndef VOLUMES_H
#define VOLUMES_H

#include <glib.h>
#include <stddef.h>
#include <stdint.h>

/** One mounted volume for scan-source UI (paths UTF-8). */
typedef struct {
  gchar *root_path;
  gchar *display_label;
} DaVolumeEntry;

void da_volume_list_free(DaVolumeEntry *entries, gsize n);

/** Fills `entries` / `n_out` with mounted volumes, sorted by root_path (letter on Windows). Caller frees via
 *  da_volume_list_free. Returns 0 on success, -1 on failure (entries unchanged). */
int da_volume_enumerate(DaVolumeEntry **entries, gsize *n_out);

/** Boot/system volume root path (e.g. `C:\` on Windows, `/` on Unix). Caller must g_free. */
gchar *da_volume_system_root_utf8(void);

/** TRUE if `path_utf8` is the same location as `volume_root_utf8` (normalized volume root). */
gboolean da_volume_is_exact_root_path(const gchar *path_utf8, const gchar *volume_root_utf8);

#if defined(_WIN32)
#include <gtk/gtk.h>
/** Clears GTK/GIO default sidebar places and adds only logical drives (A:\ …), sorted ascending. */
void da_win32_file_chooser_set_drive_places_only(GtkFileChooser *chooser);

gboolean da_win32_is_process_elevated(void);
/** ShellExecuteW "runas" restart; if @a append_elevated_marker, append ` --elevated` when absent (see app.c). */
gboolean da_win32_restart_elevated_self(gboolean append_elevated_marker);
gboolean da_win32_admin_ntfs_notice_saved_hidden(void);
/** Resolves @a path_utf8 to its volume root (e.g. "D:\\"); caller frees @a *out_root. */
gboolean da_win32_path_get_volume_root_utf8(const gchar *path_utf8, gchar **out_root);
gboolean da_win32_volume_root_is_ntfs(const gchar *volume_root_utf8);
#endif

/** Returns 0 on success; fills totals in bytes (best effort). */
int da_volume_space_for_path(const char *path_utf8, uint64_t *total, uint64_t *free_bytes,
                             uint64_t *used_bytes);

/** Fills `out` with `path_utf8` (truncated to `out_sz`). Used for combo row labels; status panel uses the raw path. */
void da_volume_selection_label(const char *path_utf8, char *out, size_t out_sz);

/**
 * Label for the status bar "selected" field: if @a path_utf8 is exactly a volume/mount root, returns
 * `[X:] VolumeName` on Windows (no backslash after the letter) or `[mount] name` on POSIX; otherwise
 * `<Folder> ` plus @a path_utf8. Caller must g_free the result. Returns NULL only when @a path_utf8 is NULL or empty.
 */
gchar *da_volume_status_selected_label(const gchar *path_utf8);

#endif  /* VOLUMES_H */
