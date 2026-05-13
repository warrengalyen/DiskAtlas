#ifndef WIN32_SHELL_CONTEXT_MENU_H
#define WIN32_SHELL_CONTEXT_MENU_H

#include <gtk/gtk.h>
#include "app_state.h"

/**
 * Remove any dynamic shell context menu items that were previously appended
 * to @a menu by da_win32_ctx_menu_refresh().  Items are identified by the
 * "da-dynamic-ctx" GObject data key.  Also releases the stored IContextMenu
 * COM pointer.  No-op on non-Win32 or if there are no dynamic items.
 */
void da_win32_remove_shell_menu_items(GtkMenu *menu);

/**
 * Remove old dynamic items from @a menu, then (on Win32, when
 * app->general_win32_explorer_context_menu is TRUE and there is a valid
 * selection) query the Windows Explorer IContextMenu for the selected paths
 * and append a GtkSeparatorMenuItem followed by one GtkMenuItem per shell
 * command to the bottom of @a menu.  No-op on non-Win32.
 */
void da_win32_ctx_menu_refresh(AppState *app, GtkMenu *menu);

#endif /* WIN32_SHELL_CONTEXT_MENU_H */
