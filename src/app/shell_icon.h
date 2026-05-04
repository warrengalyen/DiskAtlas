#ifndef SHELL_ICON_H
#define SHELL_ICON_H

#include <glib.h>
#include <gdk-pixbuf/gdk-pixbuf.h>

/**
 * Returns a new GdkPixbuf (caller must unref) with the OS-associated icon for path_utf8,
 * scaled to size_px × size_px, or NULL on failure / empty path.
 */
GdkPixbuf *da_shell_icon_for_path(const gchar *path_utf8, gint size_px);

#endif
