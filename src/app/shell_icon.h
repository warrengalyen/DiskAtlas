#ifndef SHELL_ICON_H
#define SHELL_ICON_H

#include <glib.h>
#include <gdk-pixbuf/gdk-pixbuf.h>

/**
 * Returns a new GdkPixbuf (caller must unref) with the OS-associated icon for path_utf8,
 * scaled to size_px × size_px, or NULL on failure / empty path.
 */
GdkPixbuf *da_shell_icon_for_path(const gchar *path_utf8, gint size_px);

/**
 * Returns a new GdkPixbuf (caller must unref) with the OS-associated icon for the given
 * file extension (e.g. ".mp3") or extension-less filename (e.g. "Makefile").
 * Does NOT require a real file to exist — uses type registry/content-type lookup only.
 */
GdkPixbuf *da_shell_icon_for_extension(const gchar *ext_or_name, gint size_px);

/**
 * Returns a newly-allocated string (caller must g_free) with the OS human-readable
 * description for the given file extension (e.g. ".mp3" → "MP3 audio file") or
 * extension-less filename.  Returns NULL on failure / unrecognised type.
 */
gchar *da_shell_description_for_extension(const gchar *ext_or_name);

#endif
