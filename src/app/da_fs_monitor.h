#ifndef DA_FS_MONITOR_H
#define DA_FS_MONITOR_H

#include "app_state.h"

/**
 * Start monitoring scan_root_utf8 for file-system changes using GFileMonitor.
 *
 * When a deletion or move-out is detected, the affected path is immediately
 * marked with red strikethrough in both tree views (identical to File → Delete).
 * When a creation, modification, or move-in is detected, the center status label
 * is updated to prompt the user to re-scan.
 *
 * No-op if app->general_fs_monitor is FALSE or scan_root_utf8 is empty.
 * Stops any previously active monitor before creating a new one.
 *
 * Cross-platform: GFileMonitor uses inotify on Linux, FSEvents on macOS, and
 * ReadDirectoryChangesW on Windows — all transparent through GLib/GIO.
 */
void da_fs_monitor_start(AppState *app);

/**
 * Cancel and release the active GFileMonitor, if any.
 * Sets app->fs_monitor to NULL.  Safe to call when already NULL.
 */
void da_fs_monitor_stop(AppState *app);

#endif /* DA_FS_MONITOR_H */
