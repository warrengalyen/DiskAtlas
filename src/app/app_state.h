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

/** GtkTreeStore: string columns 0..DA_COL_COUNT-1, then COL_LP (node index + 1, or negative dup group id). */
#define DA_COL_LP DA_COL_COUNT
#define DA_N_MODEL_COLS (DA_COL_COUNT + 1)

typedef struct AppState {
  GtkApplication *gtk_app;
  GtkWidget *window;
  GtkWidget *file_chooser_btn;
  GtkWidget *scan_btn;
  GtkWidget *panel_scan_label;
  GtkWidget *progress;
  GtkWidget *search;
  GtkWidget *chk_dup_mtime;
  GtkWidget *combo_display_max;
  GtkWidget *tree;
  GtkTreeStore *store;
  GtkWidget *status;
  GtkWidget *stat_sel_val;
  GtkWidget *stat_tot_val;
  GtkWidget *stat_use_val;
  GtkWidget *stat_free_val;

  scan_result_t *scan;
  guint timer_scan;
  guint timer_fill;
  guint timer_filter;
  guint timer_search;
  guint timer_tree;

  gint64 scan_start_us;
  double last_scan_elapsed_s;
  uint64_t volume_total_bytes;

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
  size_t tree_insert_pos;
  guint8 *dup_group_seen;
  size_t dup_group_seen_cap;
  size_t display_max_entries;
} AppState;

#endif  /* APP_STATE_H */
