#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#include <glib.h>
#include <gtk/gtk.h>

#include "diskatlas.h"
#include "app_state.h"
#include "flat_list_model.h"
#include "format_text.h"

/* ---- Duplicate-group child storage ---- */

/* Per-group list of child nids (all members except the representative). */
typedef struct {
  size_t *nids;   /* owned */
  size_t  count;
} FlatGroupChildren;

static void flm_group_children_free(gpointer p) {
  FlatGroupChildren *gc = (FlatGroupChildren *)p;
  if (gc) { free(gc->nids); free(gc); }
}

/* ---- GObject boilerplate ---- */

static void flat_list_model_tree_model_init(GtkTreeModelIface *iface);
static void flat_list_model_tree_sortable_init(GtkTreeSortableIface *iface);

struct _FlatListModel {
  GObject      parent;
  AppState    *app;              /* borrowed */
  size_t      *sorted;           /* top-level nids (non-dup + dup representatives) */
  size_t       count;            /* number of top-level rows */
  GHashTable  *child_map;        /* uint32_t gid → FlatGroupChildren* (owned values) */
  gboolean     has_any_children; /* TRUE when child_map has ≥1 group — fast path */
  gint         stamp;            /* iter validity stamp */
  gint         sort_col;
  GtkSortType  sort_ord;
};

G_DEFINE_TYPE_WITH_CODE(FlatListModel, flat_list_model, G_TYPE_OBJECT,
  G_IMPLEMENT_INTERFACE(GTK_TYPE_TREE_MODEL,    flat_list_model_tree_model_init)
  G_IMPLEMENT_INTERFACE(GTK_TYPE_TREE_SORTABLE, flat_list_model_tree_sortable_init)
)

/* ---- Internal helpers (placed after struct definition) ---- */

static const char *flm_basename(const char *path) {
  const char *base = path ? path : "";
  for (const char *q = base; *q; q++) {
    if (*q == '/' || *q == '\\') {
      base = q + 1;
    }
  }
  return base;
}

static uint64_t flm_dup_group_total_size(const FlatListModel *m, uint32_t gid) {
  if (m->app == NULL || m->app->scan == NULL) {
    return 0;
  }
  size_t nmem = 0;
  const size_t *mp = diskatlas_dup_group_members(m->app->scan, gid, &nmem);
  if (mp == NULL) {
    return 0;
  }
  scan_results_view_t v = scan_get_results(m->app->scan);
  if (v.nodes == NULL) {
    return 0;
  }
  uint64_t sum = 0;
  for (size_t i = 0; i < nmem; i++) {
    if (mp[i] < v.count) {
      sum += v.nodes[mp[i]].size_bytes;
    }
  }
  return sum;
}

/* ---- GObject lifecycle ---- */

static void flat_list_model_finalize(GObject *obj) {
  FlatListModel *m = FLAT_LIST_MODEL(obj);
  free(m->sorted);
  m->sorted = NULL;
  if (m->child_map) {
    g_hash_table_destroy(m->child_map);
    m->child_map = NULL;
  }
  G_OBJECT_CLASS(flat_list_model_parent_class)->finalize(obj);
}

static void flat_list_model_class_init(FlatListModelClass *klass) {
  G_OBJECT_CLASS(klass)->finalize = flat_list_model_finalize;
}

static void flat_list_model_init(FlatListModel *m) {
  m->app              = NULL;
  m->sorted           = NULL;
  m->count            = 0;
  m->child_map        = NULL;
  m->has_any_children = FALSE;
  m->stamp            = 1;
  m->sort_col         = GTK_TREE_SORTABLE_UNSORTED_SORT_COLUMN_ID;
  m->sort_ord         = GTK_SORT_DESCENDING;
}

/* ---- Iter utilities ---- */

/* user_data  = top-level row index (gintptr)
 * user_data2 = NULL → top-level row
 *            = (gintptr)(child_rank + 1) → child row at child_rank under parent */
#define ITER_ROW(iter)        ((size_t)(gintptr)((iter)->user_data))
#define ITER_IS_CHILD(iter)   ((iter)->user_data2 != NULL)
#define ITER_CHILD_RANK(iter) ((size_t)(gintptr)((iter)->user_data2) - 1u)

static void flm_set_iter(FlatListModel *m, GtkTreeIter *iter, size_t row) {
  iter->stamp      = m->stamp;
  iter->user_data  = (gpointer)(gintptr)row;
  iter->user_data2 = NULL;
  iter->user_data3 = NULL;
}

static void flm_set_child_iter(FlatListModel *m, GtkTreeIter *iter,
                                size_t parent_row, size_t child_rank) {
  iter->stamp      = m->stamp;
  iter->user_data  = (gpointer)(gintptr)parent_row;
  iter->user_data2 = (gpointer)(gintptr)(child_rank + 1u);
  iter->user_data3 = NULL;
}

static gboolean flm_iter_valid(FlatListModel *m, GtkTreeIter *iter) {
  return iter != NULL && iter->stamp == m->stamp && ITER_ROW(iter) < m->count;
}

/* Returns the FlatGroupChildren for the representative nid, or NULL. */
static FlatGroupChildren *flm_get_children(const FlatListModel *m, size_t nid) {
  if (m->child_map == NULL || m->app == NULL || m->app->scan == NULL) return NULL;
  scan_results_view_t v = scan_get_results(m->app->scan);
  if (v.nodes == NULL || nid >= v.count) return NULL;
  uint32_t gid = v.nodes[nid].duplicate_group_id;
  if (gid == DISKATLAS_DUPLICATE_GROUP_NONE) return NULL;
  return (FlatGroupChildren *)g_hash_table_lookup(m->child_map, GUINT_TO_POINTER(gid));
}

/* Returns number of children for the top-level row at 'row'. */
static size_t flm_row_child_count(const FlatListModel *m, size_t row) {
  if (!m->has_any_children || row >= m->count) return 0;
  FlatGroupChildren *gc = flm_get_children(m, m->sorted[row]);
  return gc ? gc->count : 0;
}

/* Resolves any iter (top-level or child) to a scan node index. */
static size_t flm_iter_nid(const FlatListModel *m, const GtkTreeIter *iter) {
  size_t row = ITER_ROW(iter);
  if (row >= m->count) return SIZE_MAX;
  if (!ITER_IS_CHILD(iter)) return m->sorted[row];
  FlatGroupChildren *gc = flm_get_children(m, m->sorted[row]);
  if (gc == NULL) return SIZE_MAX;
  size_t rank = ITER_CHILD_RANK(iter);
  return (rank < gc->count) ? gc->nids[rank] : SIZE_MAX;
}

/* ---- GtkTreeModelIface ---- */

static GtkTreeModelFlags flat_get_flags(GtkTreeModel *tm) {
  (void)tm;
  return GTK_TREE_MODEL_ITERS_PERSIST; /* supports 1-level nesting for dup groups */
}

static gint flat_get_n_columns(GtkTreeModel *tm) {
  (void)tm;
  return DA_N_MODEL_COLS;
}

static GType flat_get_column_type(GtkTreeModel *tm, gint col) {
  (void)tm;
  if (col >= 0 && col < DA_COL_COUNT) {
    return G_TYPE_STRING;
  }
  if (col == DA_COL_PCT) {
    return G_TYPE_INT;
  }
  if (col == DA_COL_LP) {
    return G_TYPE_INT64;
  }
  return G_TYPE_INVALID;
}

static gboolean flat_get_iter(GtkTreeModel *tm, GtkTreeIter *iter, GtkTreePath *path) {
  FlatListModel *m = FLAT_LIST_MODEL(tm);
  gint depth = gtk_tree_path_get_depth(path);
  const gint *idx = gtk_tree_path_get_indices(path);

  if (depth == 1) {
    gint row = idx[0];
    if (row < 0 || (size_t)row >= m->count) return FALSE;
    flm_set_iter(m, iter, (size_t)row);
    return TRUE;
  }
  if (depth == 2) {
    gint parent_row = idx[0];
    gint child_rank = idx[1];
    if (parent_row < 0 || (size_t)parent_row >= m->count) return FALSE;
    if (child_rank < 0) return FALSE;
    size_t nc = flm_row_child_count(m, (size_t)parent_row);
    if ((size_t)child_rank >= nc) return FALSE;
    flm_set_child_iter(m, iter, (size_t)parent_row, (size_t)child_rank);
    return TRUE;
  }
  return FALSE;
}

static GtkTreePath *flat_get_path(GtkTreeModel *tm, GtkTreeIter *iter) {
  FlatListModel *m = FLAT_LIST_MODEL(tm);
  if (!flm_iter_valid(m, iter)) return NULL;
  if (ITER_IS_CHILD(iter)) {
    return gtk_tree_path_new_from_indices(
        (gint)ITER_ROW(iter), (gint)ITER_CHILD_RANK(iter), -1);
  }
  return gtk_tree_path_new_from_indices((gint)ITER_ROW(iter), -1);
}

static void flat_get_value(GtkTreeModel *tm, GtkTreeIter *iter, gint col, GValue *val) {
  FlatListModel *m = FLAT_LIST_MODEL(tm);

  /* Always initialize val — GTK reads it regardless. */
  GType col_type = flat_get_column_type(tm, col);
  if (col_type == G_TYPE_INVALID) return;
  g_value_init(val, col_type);

  if (!flm_iter_valid(m, iter)) return;
  if (m->app == NULL || m->app->scan == NULL) return;

  size_t nid = flm_iter_nid(m, iter);
  if (nid == SIZE_MAX) return;

  scan_results_view_t v = scan_get_results(m->app->scan);
  if (v.nodes == NULL || nid >= v.count) return;
  const file_node_t *n = &v.nodes[nid];

  gboolean is_child = ITER_IS_CHILD(iter);
  char buf[512];

  switch (col) {
    case 0:
      g_value_set_string(val, flm_basename(n->path));
      break;
    case 1:
      g_value_set_string(val, n->path ? n->path : "");
      break;
    case 2:
      da_format_pct_progress_label(n->size_bytes, m->app->volume_pct_denominator_bytes,
                                   buf, sizeof(buf));
      g_value_set_string(val, buf);
      break;
    case 3:
      da_format_bytes(n->size_bytes, buf, sizeof(buf));
      g_value_set_string(val, buf);
      break;
    case 4:
      da_format_bytes(n->allocated_bytes, buf, sizeof(buf));
      g_value_set_string(val, buf);
      break;
    case 5:
      da_format_mtime_local(n->mtime_unix_ns, buf, sizeof(buf));
      g_value_set_string(val, buf);
      break;
    case 6: /* Dup Count — only shown on parent row, blank for children */
      if (!is_child && n->duplicate_group_id != DISKATLAS_DUPLICATE_GROUP_NONE) {
        FlatGroupChildren *gc = flm_get_children(m, nid);
        size_t nc = gc ? gc->count : 0u;
        snprintf(buf, sizeof(buf), "%zu", nc);
        g_value_set_string(val, buf);
      }
      break;
    case 7: /* Dup Size — only on parent row */
      if (!is_child && n->duplicate_group_id != DISKATLAS_DUPLICATE_GROUP_NONE) {
        da_format_bytes(flm_dup_group_total_size(m, n->duplicate_group_id), buf, sizeof(buf));
        g_value_set_string(val, buf);
      }
      break;
    case 8:
      da_format_win32_attr_letters(n->win32_attributes, buf, sizeof(buf));
      g_value_set_string(val, buf);
      break;
    case DA_COL_PCT:
      if (m->app->volume_pct_denominator_bytes > 0) {
        double p = 100.0 * (double)n->size_bytes / (double)m->app->volume_pct_denominator_bytes;
        if (p < 0.0) p = 0.0;
        if (p > 100.0) p = 100.0;
        g_value_set_int(val, (gint)(p + 0.5));
      } else {
        g_value_set_int(val, -1);
      }
      break;
    case DA_COL_LP:
      g_value_set_int64(val, (gint64)(nid + 1u));
      break;
    default:
      break;
  }
}

static gboolean flat_iter_next(GtkTreeModel *tm, GtkTreeIter *iter) {
  FlatListModel *m = FLAT_LIST_MODEL(tm);
  if (!flm_iter_valid(m, iter)) return FALSE;

  if (ITER_IS_CHILD(iter)) {
    /* Advance within children of the same parent. */
    size_t parent_row = ITER_ROW(iter);
    size_t next_rank  = ITER_CHILD_RANK(iter) + 1u;
    size_t nc = flm_row_child_count(m, parent_row);
    if (next_rank >= nc) return FALSE;
    flm_set_child_iter(m, iter, parent_row, next_rank);
    return TRUE;
  }

  size_t next = ITER_ROW(iter) + 1u;
  if (next >= m->count) return FALSE;
  flm_set_iter(m, iter, next);
  return TRUE;
}

static gboolean flat_iter_children(GtkTreeModel *tm, GtkTreeIter *iter,
                                    GtkTreeIter *parent) {
  FlatListModel *m = FLAT_LIST_MODEL(tm);

  if (parent == NULL) {
    /* Root children: first top-level row. */
    if (m->count == 0) return FALSE;
    flm_set_iter(m, iter, 0);
    return TRUE;
  }

  if (!flm_iter_valid(m, parent) || ITER_IS_CHILD(parent)) {
    return FALSE; /* children of children not supported */
  }
  size_t parent_row = ITER_ROW(parent);
  if (flm_row_child_count(m, parent_row) == 0) return FALSE;
  flm_set_child_iter(m, iter, parent_row, 0);
  return TRUE;
}

static gboolean flat_iter_has_child(GtkTreeModel *tm, GtkTreeIter *iter) {
  FlatListModel *m = FLAT_LIST_MODEL(tm);
  if (!flm_iter_valid(m, iter) || ITER_IS_CHILD(iter)) return FALSE;
  return flm_row_child_count(m, ITER_ROW(iter)) > 0;
}

static gint flat_iter_n_children(GtkTreeModel *tm, GtkTreeIter *iter) {
  FlatListModel *m = FLAT_LIST_MODEL(tm);
  if (iter == NULL) return (gint)m->count;
  if (!flm_iter_valid(m, iter) || ITER_IS_CHILD(iter)) return 0;
  return (gint)flm_row_child_count(m, ITER_ROW(iter));
}

static gboolean flat_iter_nth_child(GtkTreeModel *tm, GtkTreeIter *iter,
                                    GtkTreeIter *parent, gint n) {
  FlatListModel *m = FLAT_LIST_MODEL(tm);

  if (parent == NULL) {
    if (n < 0 || (size_t)n >= m->count) return FALSE;
    flm_set_iter(m, iter, (size_t)n);
    return TRUE;
  }

  if (!flm_iter_valid(m, parent) || ITER_IS_CHILD(parent)) return FALSE;
  size_t parent_row = ITER_ROW(parent);
  size_t nc = flm_row_child_count(m, parent_row);
  if (n < 0 || (size_t)n >= nc) return FALSE;
  flm_set_child_iter(m, iter, parent_row, (size_t)n);
  return TRUE;
}

static gboolean flat_iter_parent(GtkTreeModel *tm, GtkTreeIter *iter,
                                  GtkTreeIter *child) {
  FlatListModel *m = FLAT_LIST_MODEL(tm);
  if (!flm_iter_valid(m, child) || !ITER_IS_CHILD(child)) return FALSE;
  flm_set_iter(m, iter, ITER_ROW(child));
  return TRUE;
}

static void flat_ref_node(GtkTreeModel *tm, GtkTreeIter *iter) {
  (void)tm;
  (void)iter;
}

static void flat_unref_node(GtkTreeModel *tm, GtkTreeIter *iter) {
  (void)tm;
  (void)iter;
}

static void flat_list_model_tree_model_init(GtkTreeModelIface *iface) {
  iface->get_flags       = flat_get_flags;
  iface->get_n_columns   = flat_get_n_columns;
  iface->get_column_type = flat_get_column_type;
  iface->get_iter        = flat_get_iter;
  iface->get_path        = flat_get_path;
  iface->get_value       = flat_get_value;
  iface->iter_next       = flat_iter_next;
  iface->iter_children   = flat_iter_children;
  iface->iter_has_child  = flat_iter_has_child;
  iface->iter_n_children = flat_iter_n_children;
  iface->iter_nth_child  = flat_iter_nth_child;
  iface->iter_parent     = flat_iter_parent;
  iface->ref_node        = flat_ref_node;
  iface->unref_node      = flat_unref_node;
}

/* ---- Sorting ---- */

typedef struct {
  const file_node_t *nodes;
  FlatListModel     *m;
  gint               sort_col;
  GtkSortType        sort_ord;
} FlatSortCtx;

static gint flm_sort_cmp(gconstpointer a, gconstpointer b, gpointer ctx_) {
  const FlatSortCtx *ctx = (const FlatSortCtx *)ctx_;
  size_t ia = *(const size_t *)a;
  size_t ib = *(const size_t *)b;
  const file_node_t *na = &ctx->nodes[ia];
  const file_node_t *nb = &ctx->nodes[ib];
  gint r = 0;

  switch (ctx->sort_col) {
    case 0: /* Name (basename) */
      r = g_utf8_collate(flm_basename(na->path), flm_basename(nb->path));
      break;
    case 1: /* Full Path */
      r = g_utf8_collate(na->path ? na->path : "", nb->path ? nb->path : "");
      break;
    case DA_COL_PCT: /* fall-through — same key as Size */
    case 3: /* Size */
      r = (na->size_bytes < nb->size_bytes) ? -1 :
          (na->size_bytes > nb->size_bytes) ?  1 : 0;
      break;
    case 4: /* Allocated */
      r = (na->allocated_bytes < nb->allocated_bytes) ? -1 :
          (na->allocated_bytes > nb->allocated_bytes) ?  1 : 0;
      break;
    case 5: /* Modified */
      r = (na->mtime_unix_ns < nb->mtime_unix_ns) ? -1 :
          (na->mtime_unix_ns > nb->mtime_unix_ns) ?  1 : 0;
      break;
    case 6: { /* Dup Count */
      size_t ca = 0, cb = 0;
      if (ctx->m->app != NULL && ctx->m->app->scan != NULL) {
        if (na->duplicate_group_id != DISKATLAS_DUPLICATE_GROUP_NONE) {
          size_t mc = diskatlas_dup_group_member_count(ctx->m->app->scan,
                                                       na->duplicate_group_id);
          ca = mc > 0u ? mc - 1u : 0u;
        }
        if (nb->duplicate_group_id != DISKATLAS_DUPLICATE_GROUP_NONE) {
          size_t mc = diskatlas_dup_group_member_count(ctx->m->app->scan,
                                                       nb->duplicate_group_id);
          cb = mc > 0u ? mc - 1u : 0u;
        }
      }
      r = (ca < cb) ? -1 : (ca > cb) ? 1 : 0;
      break;
    }
    case 7: { /* Dup Size */
      uint64_t sa = 0, sb = 0;
      if (na->duplicate_group_id != DISKATLAS_DUPLICATE_GROUP_NONE) {
        sa = flm_dup_group_total_size(ctx->m, na->duplicate_group_id);
      }
      if (nb->duplicate_group_id != DISKATLAS_DUPLICATE_GROUP_NONE) {
        sb = flm_dup_group_total_size(ctx->m, nb->duplicate_group_id);
      }
      r = (sa < sb) ? -1 : (sa > sb) ? 1 : 0;
      break;
    }
    case 8: /* Attributes */
      r = (na->win32_attributes < nb->win32_attributes) ? -1 :
          (na->win32_attributes > nb->win32_attributes) ?  1 : 0;
      break;
    default:
      r = 0;
      break;
  }

  return (ctx->sort_ord == GTK_SORT_DESCENDING) ? -r : r;
}

/* Sort m->sorted in-place, then emit rows-reordered so GtkTreeView
 * re-renders in the new order without individual row-changed signals. */
static void flm_do_sort(FlatListModel *m) {
  if (m->count <= 1 || m->sorted == NULL) {
    return;
  }
  if (m->sort_col == GTK_TREE_SORTABLE_UNSORTED_SORT_COLUMN_ID ||
      m->sort_col == GTK_TREE_SORTABLE_DEFAULT_SORT_COLUMN_ID) {
    return;
  }
  if (m->app == NULL || m->app->scan == NULL) {
    return;
  }
  scan_results_view_t v = scan_get_results(m->app->scan);
  if (v.nodes == NULL) {
    return;
  }

  /* Save old order to compute permutation for rows-reordered. */
  size_t *old_order = (size_t *)malloc(m->count * sizeof(size_t));
  if (old_order == NULL) {
    return;
  }
  memcpy(old_order, m->sorted, m->count * sizeof(size_t));

  FlatSortCtx ctx = { v.nodes, m, m->sort_col, m->sort_ord };
  g_sort_array(m->sorted, (guint)m->count, sizeof(size_t), flm_sort_cmp, &ctx);

  /* Build permutation array: new_order[new_pos] = old_pos */
  GHashTable *pos_map = g_hash_table_new(g_direct_hash, g_direct_equal);
  for (size_t i = 0; i < m->count; i++) {
    g_hash_table_insert(pos_map,
                        (gpointer)(gintptr)old_order[i],
                        (gpointer)(gintptr)i);
  }
  gint *new_order = (gint *)malloc(m->count * sizeof(gint));
  if (new_order != NULL) {
    for (size_t i = 0; i < m->count; i++) {
      gpointer old = g_hash_table_lookup(pos_map,
                                          (gpointer)(gintptr)m->sorted[i]);
      new_order[i] = (gint)(gintptr)old;
    }
    gtk_tree_model_rows_reordered(GTK_TREE_MODEL(m), NULL, NULL, new_order);
    free(new_order);
  }
  g_hash_table_destroy(pos_map);
  free(old_order);
}

/* Quick in-place sort without computing/emitting rows-reordered.
 * Used during set_indices where GTK has not yet seen the new rows. */
static void flm_do_sort_silent(FlatListModel *m) {
  if (m->count <= 1 || m->sorted == NULL) {
    return;
  }
  if (m->sort_col == GTK_TREE_SORTABLE_UNSORTED_SORT_COLUMN_ID ||
      m->sort_col == GTK_TREE_SORTABLE_DEFAULT_SORT_COLUMN_ID) {
    return;
  }
  if (m->app == NULL || m->app->scan == NULL) {
    return;
  }
  scan_results_view_t v = scan_get_results(m->app->scan);
  if (v.nodes == NULL) {
    return;
  }
  FlatSortCtx ctx = { v.nodes, m, m->sort_col, m->sort_ord };
  g_sort_array(m->sorted, (guint)m->count, sizeof(size_t), flm_sort_cmp, &ctx);
}

/* ---- GtkTreeSortableIface ---- */

static gboolean flat_sortable_get_sort_column_id(GtkTreeSortable *sortable,
                                                  gint *sort_col,
                                                  GtkSortType *order) {
  FlatListModel *m = FLAT_LIST_MODEL(sortable);
  if (sort_col) { *sort_col = m->sort_col; }
  if (order)    { *order    = m->sort_ord; }
  return (m->sort_col != GTK_TREE_SORTABLE_UNSORTED_SORT_COLUMN_ID &&
          m->sort_col != GTK_TREE_SORTABLE_DEFAULT_SORT_COLUMN_ID);
}

static void flat_sortable_set_sort_column_id(GtkTreeSortable *sortable,
                                              gint sort_col,
                                              GtkSortType order) {
  FlatListModel *m = FLAT_LIST_MODEL(sortable);
  if (m->sort_col == sort_col && m->sort_ord == order) {
    return;
  }
  m->sort_col = sort_col;
  m->sort_ord = order;
  flm_do_sort(m);
  gtk_tree_sortable_sort_column_changed(sortable);
}

static void flat_sortable_set_sort_func(GtkTreeSortable *sortable, gint sort_col,
                                        GtkTreeIterCompareFunc func,
                                        gpointer data,
                                        GDestroyNotify destroy) {
  /* FlatListModel uses its own direct comparators; discard external funcs. */
  (void)sortable;
  (void)sort_col;
  (void)func;
  if (destroy != NULL && data != NULL) {
    destroy(data);
  }
}

static void flat_sortable_set_default_sort_func(GtkTreeSortable *sortable,
                                                 GtkTreeIterCompareFunc func,
                                                 gpointer data,
                                                 GDestroyNotify destroy) {
  (void)sortable;
  (void)func;
  if (destroy != NULL && data != NULL) {
    destroy(data);
  }
}

static gboolean flat_sortable_has_default_sort_func(GtkTreeSortable *sortable) {
  (void)sortable;
  return FALSE;
}

static void flat_list_model_tree_sortable_init(GtkTreeSortableIface *iface) {
  iface->get_sort_column_id    = flat_sortable_get_sort_column_id;
  iface->set_sort_column_id    = flat_sortable_set_sort_column_id;
  iface->set_sort_func         = flat_sortable_set_sort_func;
  iface->set_default_sort_func = flat_sortable_set_default_sort_func;
  iface->has_default_sort_func = flat_sortable_has_default_sort_func;
}

/* ---- Public API ---- */

FlatListModel *flat_list_model_new(AppState *app) {
  FlatListModel *m = g_object_new(FLAT_LIST_TYPE_MODEL, NULL);
  m->app = app;
  return m;
}

void flat_list_model_set_indices(FlatListModel *m, const size_t *indices, size_t count) {
  g_return_if_fail(FLAT_LIST_IS_MODEL(m));

  GtkTreeModel *tm = GTK_TREE_MODEL(m);
  size_t old_count = m->count;

  /* ---- Build new sorted[] and child_map ---- */

  gboolean have_scan = (m->app != NULL && m->app->scan != NULL);
  scan_results_view_t v;
  if (have_scan) {
    v = scan_get_results(m->app->scan);
    have_scan = (v.nodes != NULL);
  }

  size_t *new_sorted = NULL;
  size_t  new_count  = 0;
  GHashTable *new_child_map = g_hash_table_new_full(
      NULL, NULL, NULL, flm_group_children_free);

  if (count > 0 && indices != NULL) {
    /* We need at most 'count' top-level slots (could be fewer if some are dup children). */
    new_sorted = (size_t *)malloc(count * sizeof(size_t));
    if (new_sorted != NULL && have_scan) {
      /* seen: gid → (gpointer)1 once the representative has been placed */
      GHashTable *seen = g_hash_table_new(NULL, NULL);
      /* temp_children: gid → GArray of child size_t nids */
      GHashTable *temp = g_hash_table_new_full(NULL, NULL, NULL,
                                               (GDestroyNotify)g_array_unref);

      for (size_t i = 0; i < count; i++) {
        size_t nid = indices[i];
        if (nid >= v.count) continue;
        uint32_t gid = v.nodes[nid].duplicate_group_id;

        if (gid == DISKATLAS_DUPLICATE_GROUP_NONE) {
          new_sorted[new_count++] = nid;
        } else {
          if (!g_hash_table_contains(seen, GUINT_TO_POINTER(gid))) {
            /* First occurrence → representative / parent row */
            g_hash_table_insert(seen, GUINT_TO_POINTER(gid), GUINT_TO_POINTER(1));
            new_sorted[new_count++] = nid;
          } else {
            /* Subsequent occurrence → child of its representative */
            GArray *arr = (GArray *)g_hash_table_lookup(temp, GUINT_TO_POINTER(gid));
            if (arr == NULL) {
              arr = g_array_new(FALSE, FALSE, sizeof(size_t));
              g_hash_table_insert(temp, GUINT_TO_POINTER(gid), arr);
            }
            g_array_append_val(arr, nid);
          }
        }
      }

      /* Materialise temp arrays into new_child_map */
      GHashTableIter git;
      gpointer key, val2;
      g_hash_table_iter_init(&git, temp);
      while (g_hash_table_iter_next(&git, &key, &val2)) {
        GArray *arr = (GArray *)val2;
        if (arr->len == 0) continue;
        FlatGroupChildren *gc = (FlatGroupChildren *)malloc(sizeof(FlatGroupChildren));
        if (gc) {
          gc->count = arr->len;
          gc->nids  = (size_t *)malloc(arr->len * sizeof(size_t));
          if (gc->nids) {
            memcpy(gc->nids, arr->data, arr->len * sizeof(size_t));
          } else {
            gc->count = 0;
          }
          g_hash_table_insert(new_child_map, key, gc);
        }
      }
      g_hash_table_destroy(temp);
      g_hash_table_destroy(seen);

    } else if (new_sorted != NULL) {
      /* No scan info: all indices are top-level. */
      memcpy(new_sorted, indices, count * sizeof(size_t));
      new_count = count;
    }
  }

  /* Swap old data for new */
  free(m->sorted);
  if (m->child_map) g_hash_table_destroy(m->child_map);
  m->sorted           = new_sorted;
  m->count            = new_count;
  m->child_map        = new_child_map;
  m->has_any_children = (g_hash_table_size(new_child_map) > 0);

  /* Apply existing sort order silently before notifying GTK. */
  flm_do_sort_silent(m);

  /* Invalidate existing iters. */
  m->stamp++;

  /* ---- Notify GTK ---- */
  GtkTreePath *path = gtk_tree_path_new();
  gtk_tree_path_append_index(path, 0);
  gint *pidx = gtk_tree_path_get_indices(path);
  GtkTreeIter iter;

  /* Delete rows that no longer exist (shrunk). */
  for (size_t i = old_count; i > m->count; i--) {
    pidx[0] = (gint)(i - 1u);
    gtk_tree_model_row_deleted(tm, path);
  }

  /* row-changed for rows in both old and new data. */
  size_t common = old_count < m->count ? old_count : m->count;
  for (size_t i = 0; i < common; i++) {
    pidx[0] = (gint)i;
    flm_set_iter(m, &iter, i);
    gtk_tree_model_row_changed(tm, path, &iter);
    /* Tell GTK if child-presence changed for this row. */
    gtk_tree_model_row_has_child_toggled(tm, path, &iter);
  }

  /* Insert rows beyond old_count (grew). */
  for (size_t i = old_count; i < m->count; i++) {
    pidx[0] = (gint)i;
    flm_set_iter(m, &iter, i);
    gtk_tree_model_row_inserted(tm, path, &iter);
    /* GTK calls iter_has_child after row-inserted, but emit anyway to be safe. */
    if (flm_row_child_count(m, i) > 0) {
      gtk_tree_model_row_has_child_toggled(tm, path, &iter);
    }
  }

  gtk_tree_path_free(path);
}

void flat_list_model_invalidate(FlatListModel *m) {
  g_return_if_fail(FLAT_LIST_IS_MODEL(m));
  if (m->count == 0) return;

  GtkTreeModel *tm   = GTK_TREE_MODEL(m);
  GtkTreeIter   iter;

  for (size_t i = 0; i < m->count; i++) {
    GtkTreePath *path = gtk_tree_path_new_from_indices((gint)i, -1);
    flm_set_iter(m, &iter, i);
    gtk_tree_model_row_changed(tm, path, &iter);

    size_t nc = flm_row_child_count(m, i);
    for (size_t c = 0; c < nc; c++) {
      GtkTreePath *cpath = gtk_tree_path_new_from_indices((gint)i, (gint)c, -1);
      flm_set_child_iter(m, &iter, i, c);
      gtk_tree_model_row_changed(tm, cpath, &iter);
      gtk_tree_path_free(cpath);
    }
    gtk_tree_path_free(path);
  }
}
