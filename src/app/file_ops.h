#ifndef FILE_OPS_H
#define FILE_OPS_H

#include <stddef.h>
#include <glib.h>
#include <gtk/gtk.h>

/**
 * Place a copy of the given paths on the system clipboard so they can be
 * pasted into a file manager (Ctrl+V / paste).
 * @param window  Parent GTK window (used to get the display's clipboard).
 * @param paths   UTF-8 file/directory paths.
 * @param count   Number of entries in @a paths.
 * @return TRUE on success.
 */
gboolean da_file_op_copy_to_clipboard(GtkWidget *window, const char **paths, size_t count);

/**
 * Place the given paths on the system clipboard for a move (cut) operation.
 * Pasting them in a file manager will move the files.
 */
gboolean da_file_op_cut_to_clipboard(GtkWidget *window, const char **paths, size_t count);

/**
 * Move each path to the system trash / recycle bin.
 * @param paths  UTF-8 paths to trash.
 * @param count  Number of paths.
 * @param error  Set on the first failure; caller must free with g_error_free().
 * @return TRUE if every operation succeeded.
 */
gboolean da_file_op_trash(const char **paths, size_t count, GError **error);

/**
 * Permanently delete each path (non-recoverable).  For directories the
 * entire subtree is removed.
 * @param paths  UTF-8 paths to delete.
 * @param count  Number of paths.
 * @param error  Set on the first failure; caller must free with g_error_free().
 * @return TRUE if every operation succeeded.
 */
gboolean da_file_op_delete_permanent(const char **paths, size_t count, GError **error);

#endif /* FILE_OPS_H */
