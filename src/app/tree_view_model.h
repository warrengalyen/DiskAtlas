#ifndef TREE_VIEW_MODEL_H
#define TREE_VIEW_MODEL_H

#include <gtk/gtk.h>
#include "app_state.h"

/* ---- GtkTreeStore column indices for the Tree View tab ---- */
#define DA_TV_COL_NAME       0   /* string: basename */
#define DA_TV_COL_PATH       1   /* string: full path (for shell icon lookup) */
#define DA_TV_COL_PCT_LABEL  2   /* string: "X.X %" label */
#define DA_TV_COL_SIZE       3   /* string: formatted logical size */
#define DA_TV_COL_ALLOC      4   /* string: formatted allocated bytes */
#define DA_TV_COL_ITEMS      5   /* string: files + folders count */
#define DA_TV_COL_FILES      6   /* string: file descendant count */
#define DA_TV_COL_FOLDERS    7   /* string: folder descendant count */
#define DA_TV_COL_MODIFIED   8   /* string: formatted mtime */
#define DA_TV_COL_ATTRS      9   /* string: Win32 attribute letters */
#define DA_TV_COL_PCT_VAL   10   /* gint: 0–100 for progress bar, -1 = N/A */
#define DA_TV_COL_IDX_ID    11   /* gint64: entry index into DaTreeViewModel, or DA_TV_LP_PLACEHOLDER */
#define DA_TV_COL_KIND      12   /* guint: DISKATLAS_NODE_KIND_* */
#define DA_TV_N_COLS        13

/** Sentinel value in DA_TV_COL_IDX_ID: row is a lazy-expand placeholder awaiting population. */
#define DA_TV_LP_PLACEHOLDER (-1LL)

/** Opaque tree model containing the pre-built flat entry array with tree links. */
typedef struct DaTreeViewModel DaTreeViewModel;

/** Allocate a new GtkTreeStore with the tree view column schema. */
GtkTreeStore *da_tree_view_store_new(void);

/**
 * Build the hierarchical tree model from the current scan results and
 * populate the root level of app->tree_view_store.  Children are loaded
 * lazily via da_tree_view_on_row_expanded.
 */
void da_tree_view_populate(AppState *app);

/** Clear the tree view store and release the internal entry array. */
void da_tree_view_clear(AppState *app);

/** Recompute Size / Alloc / % columns from raw entry stats (e.g. after size decimal-place preference changes). */
void da_tree_view_refresh_size_columns(AppState *app);

/**
 * GtkTreeView "row-expanded" signal handler.  Replaces the placeholder
 * child row with the actual children of the expanded entry.
 */
void da_tree_view_on_row_expanded(GtkTreeView *tv, GtkTreeIter *iter,
                                  GtkTreePath *path, gpointer user_data);

/**
 * Return the pre-aggregated stats for a tree view entry by its entry index
 * (the value stored in DA_TV_COL_IDX_ID).  Returns FALSE if the model is
 * NULL, the index is out of range, or it is a placeholder row.
 *
 * For files:  size_bytes = own size, file_count = 1.
 * For dirs:   size_bytes = subtree total, file_count = total descendant files.
 */
gboolean da_tv_entry_get_stats(const DaTreeViewModel *m, gint64 entry_id,
                               uint64_t *out_size, uint64_t *out_alloc,
                               uint64_t *out_file_count);

#endif /* TREE_VIEW_MODEL_H */
