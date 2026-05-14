#ifndef APP_STATE_H
#define APP_STATE_H

#include <gtk/gtk.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "diskatlas.h"

#include "dm_treemap_colors.h"

/** Opaque tree-view model (defined in tree_view_model.c). */
typedef struct DaTreeViewModel DaTreeViewModel;

/** Forward declaration — full type in flat_list_model.h. */
typedef struct _FlatListModel FlatListModel;

/** Forward declaration — full type in dm_mime_db.h. */
struct DmMimeDatabase;

#define DA_COL_COUNT 9
/** String column index: formatted allocated size (matches file list `titles` / model column 4). */
#define DA_COL_ALLOCATED 4
#define DA_TREEINSERT_BATCH 960
#define DA_TREEINSERT_MS 22
#define DA_FILTER_BATCH 4000
#define DA_SEARCH_DEBOUNCE_MS 200

/** GtkTreeStore: string cols 0..DA_COL_COUNT-1; DA_COL_PCT bar value (gint -1 = N/A, else 0–100); DA_COL_LP
 *  (node index + 1, or negative dup group id). */
#define DA_COL_PCT DA_COL_COUNT
#define DA_COL_LP (DA_COL_COUNT + 1)
#define DA_N_MODEL_COLS (DA_COL_COUNT + 2)

typedef struct AppState {
  GtkApplication *gtk_app;
  GtkWidget *window;
  /** Top bar (volume combo, scan, stats); visibility toggled via Options → Show header. */
  GtkWidget *header_panel;
  /** Right pane of Tree View tab (file-type list); toggled via Options → Show File Types. */
  GtkWidget *file_type_scrolled;
  /** Lower pane of Tree View tab (treemap + caption); toggled via Options → Show Treemap. */
  GtkWidget *treemap_panel;
  /** File menu "Export to CSV…"; sensitivity synced to exportable scan state. */
  GtkWidget *file_menu_export_csv;
  /** File menu zoom in/out; sensitivity synced when a scan with a treemap is active. */
  GtkWidget *file_menu_zoom_in;
  GtkWidget *file_menu_zoom_out;
  /** File menu "Copy file and size info…"; same sensitivity as export CSV. */
  GtkWidget *file_menu_copy_clipboard;
  GtkWidget *file_menu_explore_folder;
  GtkWidget *file_menu_terminal;
  GtkWidget *file_menu_copy_path;
  /** File menu copy/cut/delete items; sensitivity requires a scan selection. */
  GtkWidget *file_menu_copy;
  GtkWidget *file_menu_cut;
  GtkWidget *file_menu_delete_trash;
  GtkWidget *file_menu_delete_permanent;
  GtkWidget *file_menu_rename;
  /** Standalone context menu shown on right-click in treemap/file-view/tree-view. */
  GtkWidget *context_menu;
  GtkWidget *context_menu_explore_folder;
  GtkWidget *context_menu_terminal_here;
  GtkWidget *context_menu_copy_path;
  GtkWidget *context_menu_export_csv;
  GtkWidget *context_menu_copy_file_info;
  GtkWidget *context_menu_zoom_separator;
  GtkWidget *context_menu_zoom_in;
  GtkWidget *context_menu_zoom_out;
  GtkWidget *scan_source_combo;
  gint scan_source_last_stable_active;
  GtkWidget *scan_btn;
  GtkWidget *panel_scan_label;
  GtkWidget *progress;
  GtkWidget *search;
  GtkWidget *duplicates_file_combo;
  GtkWidget *match_filename_only_radio;
  GtkWidget *match_entire_path_radio;
  GtkWidget *duplicates_only_check;
  GtkWidget *show_folders_check;
  GtkWidget *combo_display_max;
  GtkWidget *tree;
  /** Centered caption above treemap (“Top level: …”). */
  GtkWidget *treemap_panel_title;
  GtkWidget *treemap;
  /** Flat custom GtkTreeModel for the file-view list. */
  FlatListModel *flat_list_model;
  GtkWidget *main_notebook;
  GtkWidget *status_label_left;
  GtkWidget *status_label_center;
  GtkWidget *status_label_right;
  GtkWidget *stat_sel_val;
  GtkWidget *stat_tot_val;
  GtkWidget *stat_use_val;
  GtkWidget *stat_free_val;

  /** Windows: top banner prompting elevation for NTFS MFT scan; NULL if not loaded. */
  GtkWidget *admin_ntfs_notice_panel;
  GtkWidget *restart_admin_btn;
  GtkWidget *dont_show_again_check;

  /** Tree View tab widgets and model. */
  GtkWidget *tree_view;
  GtkTreeStore *tree_view_store;
  DaTreeViewModel *tree_view_model;
  gboolean tree_view_populated;
  /** Flat file-type stats treeview (right pane of tree_view_top_paned). */
  GtkWidget *file_type_tree;
  /** GCancellable for the background tree-view build GTask; NULL when idle. */
  GCancellable *tv_build_cancel;
  /** Scan kept alive while the background tree-view build worker is running. */
  scan_result_t *tv_held_scan;
  /** When TRUE, `tv_build_done` will call `da_fs_monitor_scan_phase_end` after the GTask finishes. */
  gboolean defer_fs_monitor_phase_end;
  /** TRUE from `da_tree_view_populate` scheduling a GTask until `tv_build_done` finishes (worker + GTK). */
  gboolean tv_build_worker_pending;

  scan_result_t *scan;
  guint timer_scan;
  guint timer_fill;
  guint timer_filter;
  guint timer_search;

  gint64 scan_start_us;
  double last_scan_elapsed_s;
  uint64_t volume_total_bytes;
  /** Denominator for file list "%" column: used bytes on volume (WizTree-style); equals
   *  volume_total_bytes when used is unknown or zero. */
  uint64_t volume_pct_denominator_bytes;

  char *scan_root_utf8;
  /** After CSV import: full path to the .csv file; combo stays on "<CSV File>" row. */
  gchar *csv_import_path;
  /** Volume/common root from imported paths (e.g. "D:\\"); tree view + treemap root when scan_root_utf8 is "". */
  gchar *csv_derived_root_utf8;
  gboolean csv_import_active;
  /** TRUE when the active snapshot was loaded from a DiskAtlas binary scan index file (.mft with index magic; vs CSV). */
  /** When csv_import_active: TRUE if snapshot came from a raw $MFT dump (vs CSV). */
  gboolean import_snapshot_is_raw_mft;
  size_t *master_indices;
  size_t master_count;
  size_t *filtered_indices;
  size_t filtered_count;
  size_t filtered_cap;
  gchar filter_text[512];
  /** Newest-first search strings for file-view combo; NULL until search combo is initialized. */
  GPtrArray *search_history;
  gboolean filter_active;
  size_t filter_scan_pos;
  gboolean filter_build_running;
  size_t populate_total;
  gboolean list_populated;
  /** Guard flag to prevent re-entrant A→B→A sync loops between treemap and tree_view. */
  gboolean treemap_tree_sync_in_progress;
  size_t display_max_entries;

  /** Formatted size strings (KiB+): decimal fraction digits, 0–4; persisted under `[interface]` in diskatlas.ini. */
  gint size_decimal_places;

  /** `DaSizeDisplayFormat`: dynamic / bytes / KB / … ; persisted as `size_display_format` in `[interface]`. */
  gint interface_size_display_format;

  /** Treemap lighting/borders; `enable_tile_gradients` persisted as `treemap_tile_gradients` in `[interface]`. */
  DmTreemapStyle treemap_style;

  /** Zebra striping for tree views; persisted as `alternate_row_colors` in `[interface]`. Default FALSE. */
  gboolean interface_alternate_row_colors;

  /** When TRUE, `header_panel` is visible; persisted as `show_header` in `[interface]`. Default TRUE. */
  gboolean interface_show_header;

  /** When TRUE, `file_type_scrolled` is visible; persisted as `show_file_types` in `[interface]`. Default TRUE. */
  gboolean interface_show_file_types;

  /** When TRUE, `treemap_panel` is visible; persisted as `show_treemap` in `[interface]`. Default TRUE. */
  gboolean interface_show_treemap;

  /** When TRUE, treemap renders a free space tile; persisted as `treemap_show_free_space` in `[interface]`. Default FALSE. */
  gboolean interface_treemap_show_free_space;

  /** When TRUE, treemap renders file/folder name labels on tiles; persisted as `treemap_show_labels` in `[interface]`. Default TRUE. */
  gboolean interface_treemap_show_labels;

  /** Cached free bytes from the most-recent volume query (used for free-space tile). 0 when unknown. */
  uint64_t volume_free_bytes;

  /** When non-NULL, the treemap is zoomed to this subdirectory instead of scan_root_utf8. Owned. */
  gchar *treemap_zoom_root_utf8;

  /** When TRUE, File → Rename, F2, and inline name editing are allowed; persisted as `enable_rename` in `[general]`. */
  gboolean general_enable_rename;
  /** When TRUE (Win32 only), Windows Explorer context menu is appended to the
   *  right-click context menu; persisted as `win32_explorer_context_menu` in `[general]`. Default TRUE. */
  gboolean general_win32_explorer_context_menu;

  /** When TRUE, double-click on a file row in Tree View / File View opens it with the system default app;
   *  persisted as `open_file_double_click` in `[general]`. Default TRUE. */
  gboolean general_open_file_double_click;

  /** When TRUE, a GFileMonitor watches scan_root_utf8 after each scan; detected deletions are
   *  immediately marked with red strikethrough and other changes trigger a status bar notice.
   *  Persisted as `monitor_file_system` in `[general]`. Default TRUE. */
  gboolean general_fs_monitor;

  /** TRUE from `da_fs_monitor_scan_phase_begin` until `da_fs_monitor_scan_phase_end`: blocks
   *  `da_fs_monitor_start` (including from settings) and ignores stray monitor callbacks so the
   *  live watcher does not run while a scan or list rebuild is in progress. */
  gboolean fs_monitor_pause_for_scan;

  /** When TRUE, column-0 rows in file_view_tree and tree_view_tree can be dragged out to the OS
   *  file manager (move by default, copy when Ctrl is held).
   *  Persisted as `enable_drag_and_drop` in `[general]`. Default TRUE. */
  gboolean general_enable_drag_drop;

  /** Live GFileMonitor watching scan_root_utf8; NULL when inactive.
   *  Owned: cancel + unref via da_fs_monitor_stop(). */
  GFileMonitor *fs_monitor;

  /** Transient: second-click rename gesture on column 0 (see `da_ui_cancel_pending_name_rename`). */
  GtkTreePath *name_rename_candidate_path;
  GtkTreeView *name_rename_candidate_tv;
  guint name_rename_pending_timeout;

  /** Runtime MIME classification database; built at startup and rebuilt after settings changes. */
  struct DmMimeDatabase *mime_db;

  /**
   * Set of UTF-8 paths that have been visually marked as deleted (moved to
   * trash or permanently deleted) since the last scan.  All descendants of a
   * deleted directory are included.  gchar* keys owned by the table; value
   * pointer is unused.  NULL until the first deletion occurs.
   * Freed and set to NULL when a new scan or import replaces app->scan.
   */
  GHashTable *deleted_path_set;

  gchar *mft_dump_save_path;
  gchar *mft_dump_volume_root_utf8;
  gboolean mft_dump_run_stream_after_scan;
  gboolean mft_dump_custom_scan_panel;
  gboolean mft_dump_internal_scan;
  uint64_t mft_dump_size_total_hint;
  gboolean mft_dump_banner_after_populate;
} AppState;

#endif  /* APP_STATE_H */
