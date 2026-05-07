#ifndef SCAN_SOURCE_COMBO_H
#define SCAN_SOURCE_COMBO_H

#include "app_state.h"

#include <gtk/gtk.h>

void da_scan_source_combo_setup(AppState *app);
void da_scan_source_combo_rebuild(AppState *app);
void da_scan_source_combo_on_changed(GtkComboBox *cb, gpointer user_data);

/** Optional: silence GLib-GIO CRITICAL spam from GtkFileChooserNative/IFileDialog on Windows (GLib 2.84+).
 *  Safe to call multiple times; installs at most once. */
void da_scan_source_install_gio_log_filter_once(void);

#endif
