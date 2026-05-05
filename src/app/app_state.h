#ifndef APP_STATE_H
#define APP_STATE_H

#include <gtk/gtk.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "diskatlas.h"

#define DA_COL_COUNT 9
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
  GtkWidget *file_chooser_btn;
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
  GtkTreeStore *store;
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

  scan_result_t *scan;
  guint timer_scan;
  guint timer_fill;
  guint timer_filter;
  guint timer_search;
  guint timer_tree;

  gint64 scan_start_us;
  double last_scan_elapsed_s;
  uint64_t volume_total_bytes;
  /** Denominator for file list "%" column: used bytes on volume (WizTree-style); equals
   *  volume_total_bytes when used is unknown or zero. */
  uint64_t volume_pct_denominator_bytes;

  char *scan_root_utf8;
  size_t *master_indices;
  size_t master_count;
  size_t *filtered_indices;
  size_t filtered_count;
  size_t filtered_cap;
  gchar filter_text[512];
  gboolean filter_active;
  size_t filter_scan_pos;
  gboolean filter_build_running;
  size_t populate_total;
  gboolean list_populated;
  /** TRUE when chunked GtkTreeView root insert is not running (see da_tree_begin_root_insert / timer_tree). */
  gboolean file_view_tree_ready;
  size_t tree_insert_pos;
  guint8 *dup_group_seen;
  size_t dup_group_seen_cap;
  size_t display_max_entries;
} AppState;

#endif  /* APP_STATE_H */
