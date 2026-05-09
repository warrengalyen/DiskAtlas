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
  /** File menu "Export to CSV…"; sensitivity synced to exportable scan state. */
  GtkWidget *file_menu_export_csv;
  /** File menu "Copy file and size info…"; same sensitivity as export CSV. */
  GtkWidget *file_menu_copy_clipboard;
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
  /** GCancellable for the background tree-view build GTask; NULL when idle. */
  GCancellable *tv_build_cancel;
  /** Scan kept alive while the background tree-view build worker is running. */
  scan_result_t *tv_held_scan;

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

  /** Treemap lighting/borders; `enable_tile_gradients` persisted as `treemap_tile_gradients` in `[interface]`. */
  DmTreemapStyle treemap_style;

  /** Runtime MIME classification database; built at startup and rebuilt after settings changes. */
  struct DmMimeDatabase *mime_db;

  gchar *mft_dump_save_path;
  gchar *mft_dump_volume_root_utf8;
  gboolean mft_dump_run_stream_after_scan;
  gboolean mft_dump_custom_scan_panel;
  gboolean mft_dump_internal_scan;
  uint64_t mft_dump_size_total_hint;
  gboolean mft_dump_banner_after_populate;
} AppState;

#endif  /* APP_STATE_H */
