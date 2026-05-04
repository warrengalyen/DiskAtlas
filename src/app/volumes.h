#ifndef VOLUMES_H
#define VOLUMES_H

#include <stddef.h>
#include <stdint.h>

#if defined(_WIN32)
#include <gtk/gtk.h>
/** Clears GTK/GIO default sidebar places and adds only logical drives (A:\ …), sorted ascending. */
void da_win32_file_chooser_set_drive_places_only(GtkFileChooser *chooser);
#endif

/** Returns 0 on success; fills totals in bytes (best effort). */
int da_volume_space_for_path(const char *path_utf8, uint64_t *total, uint64_t *free_bytes,
                             uint64_t *used_bytes);

/** Fills `out` with a short selection label, e.g. "[D:] Storage" on Windows for paths on that drive;
 *  otherwise copies `path_utf8`. */
void da_volume_selection_label(const char *path_utf8, char *out, size_t out_sz);

#endif
