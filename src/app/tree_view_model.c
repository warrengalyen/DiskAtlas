#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stddef.h>

#include <glib.h>
#include <gtk/gtk.h>

#include "diskatlas.h"
#include "tree_view_model.h"
#include "format_text.h"
#include "da_fs_monitor.h"

/* ---- Internal entry (one per scan node, in depth-sorted order) ---- */

typedef struct {
  uint32_t kind;             /* DISKATLAS_NODE_KIND_* */
  const char *path;          /* borrowed from scan blob (valid while app->scan alive) */
  size_t      scan_nid;      /* SIZE_MAX = synthetic root row (no scan node) */
  uint64_t mtime_unix_ns;
  uint32_t win32_attributes;
  uint64_t size_bytes;       /* file: own size; dir: subtree logical total */
  uint64_t alloc_bytes;      /* file: own alloc; dir: subtree alloc total */
  uint64_t file_count;       /* # file descendants (self = 1 for plain files) */
  uint64_t folder_count;     /* # folder descendants recursively (not self) */
  int32_t parent_id;         /* index in entries[], or -1 for root */
  int32_t first_child_id;    /* head of singly-linked child list, or -1 */
  int32_t next_sibling_id;   /* next sibling in parent's child list, or -1 */
  int32_t last_child_id;     /* tail of child list for O(1) append, or -1 */
} DaTvEntry;

struct DaTreeViewModel {
  DaTvEntry *entries;
  size_t     count;
  char      *scan_root_str; /* owns the string that synthetic root entry->path points to */
};

/* ---- Path utilities ---- */

/**
 * Return the display name for a path.  For most paths this is the final
 * component (basename).  If the basename would be empty — e.g. "D:\" where
 * the trailing separator leaves nothing — fall back to the full path so the
 * row always shows something useful.
 */
static const char *tv_display_name(const char *path) {
  if (path == NULL || path[0] == '\0') {
    return "";
  }
  const char *base = path;
  for (const char *q = path; *q; q++) {
    if (*q == '/' || *q == '\\') {
      base = q + 1;
    }
  }
  return (base[0] != '\0') ? base : path;
}

static int count_separators(const char *path) {
  int n = 0;
  if (!path) {
    return 0;
  }
  for (const char *p = path; *p; p++) {
    if (*p == '/' || *p == '\\') {
      n++;
    }
  }
  return n;
}

/**
 * Returns a g_malloc'd parent path string, or NULL if path has no parent
 * (i.e., it is a filesystem root or has no separator).
 */
static gchar *compute_parent_path(const char *path) {
  if (path == NULL || path[0] == '\0') {
    return NULL;
  }

  const char *last_sep = NULL;
  for (const char *p = path; *p; p++) {
    if (*p == '/' || *p == '\\') {
      last_sep = p;
    }
  }

  if (last_sep == NULL) {
    return NULL;
  }

  /* Trailing separator means path itself is a root (e.g. "D:\", "/"). */
  if (*(last_sep + 1) == '\0') {
    return NULL;
  }

  size_t len = (size_t)(last_sep - path);

  /* Windows drive root: "C:\foo" → last_sep is the backslash at offset 2.
   * Parent should be "C:\" (include the separator). */
  if (len == 2 && path[1] == ':') {
    char sep = *last_sep;
    return g_strdup_printf("%c:%c", path[0], sep);
  }

  /* Unix root child: "/foo" → len == 0, parent is "/". */
  if (len == 0) {
    return g_strdup("/");
  }

  return g_strndup(path, len);
}

/* ---- Sort comparator (by path depth, then alphabetically) ---- */

/* Pre-computed depth table: depths[node_index] = separator count, capped at 255. */
static const file_node_t *tv_sort_nodes;
static const uint8_t     *tv_sort_depths;

static int cmp_by_depth(const void *a, const void *b) {
  size_t ia = *(const size_t *)a;
  size_t ib = *(const size_t *)b;
  int da = tv_sort_depths ? (int)tv_sort_depths[ia] : count_separators(tv_sort_nodes[ia].path);
  int db = tv_sort_depths ? (int)tv_sort_depths[ib] : count_separators(tv_sort_nodes[ib].path);
  if (da != db) {
    return da - db;
  }
  return g_strcmp0(tv_sort_nodes[ia].path, tv_sort_nodes[ib].path);
}

/* ---- GtkTreeStore schema ---- */

GtkTreeStore *da_tree_view_store_new(void) {
  return gtk_tree_store_new(DA_TV_N_COLS,
    G_TYPE_STRING,  /* DA_TV_COL_NAME */
    G_TYPE_STRING,  /* DA_TV_COL_PATH */
    G_TYPE_STRING,  /* DA_TV_COL_PCT_LABEL */
    G_TYPE_STRING,  /* DA_TV_COL_SIZE */
    G_TYPE_STRING,  /* DA_TV_COL_ALLOC */
    G_TYPE_STRING,  /* DA_TV_COL_ITEMS */
    G_TYPE_STRING,  /* DA_TV_COL_FILES */
    G_TYPE_STRING,  /* DA_TV_COL_FOLDERS */
    G_TYPE_STRING,  /* DA_TV_COL_MODIFIED */
    G_TYPE_STRING,  /* DA_TV_COL_ATTRS */
    G_TYPE_INT,     /* DA_TV_COL_PCT_VAL */
    G_TYPE_INT64,   /* DA_TV_COL_IDX_ID */
    G_TYPE_UINT     /* DA_TV_COL_KIND */
  );
}

/* ---- Insert a single entry (and placeholder if it has children) ---- */

/** Fills formatted strings for tree-view columns derived from entry @a eid. */
static void da_tv_format_row_strings(const DaTreeViewModel *m, int32_t eid, char *pct_label,
                                     size_t pct_sz, char *size_str, size_t size_sz, char *alloc_str,
                                     size_t alloc_sz, gint *out_pct_val) {
  const DaTvEntry *e = &m->entries[eid];

  gint pct_val = -1;
  if (e->parent_id >= 0) {
    uint64_t psz = m->entries[e->parent_id].size_bytes;
    da_format_pct_progress_label(e->size_bytes, psz, pct_label, pct_sz);
    if (psz > 0) {
      double p = (double)e->size_bytes / (double)psz * 100.0;
      pct_val = (p > 100.0) ? 100 : (gint)p;
    }
  } else {
    pct_val = 100;
    g_strlcpy(pct_label, "100.0 %", pct_sz);
  }
  if (out_pct_val != NULL) {
    *out_pct_val = pct_val;
  }

  da_format_bytes(e->size_bytes, size_str, size_sz);
  da_format_bytes(e->alloc_bytes, alloc_str, alloc_sz);
}

static void da_tv_insert_entry(AppState *app, GtkTreeIter *parent_iter, int32_t eid) {
  DaTreeViewModel *m = app->tree_view_model;
  char pct_label[48];
  char size_str[64], alloc_str[64];
  char items_str[64], files_str[64], folders_str[64], mtime_str[64], attrs_str[32];
  gint pct_val;

  da_tv_format_row_strings(m, eid, pct_label, sizeof(pct_label), size_str, sizeof(size_str), alloc_str,
                           sizeof(alloc_str), &pct_val);

  const DaTvEntry *e = &m->entries[eid];

  da_format_uint64_locale(e->file_count + e->folder_count, items_str, sizeof(items_str));
  da_format_uint64_locale(e->file_count, files_str, sizeof(files_str));
  da_format_uint64_locale(e->folder_count, folders_str, sizeof(folders_str));
  da_format_mtime_local(e->mtime_unix_ns, mtime_str, sizeof(mtime_str));
  da_format_win32_attr_letters(e->win32_attributes, attrs_str, sizeof(attrs_str));

  const char *name = tv_display_name(e->path);

  GtkTreeIter iter;
  gtk_tree_store_insert_with_values(app->tree_view_store, &iter, parent_iter, -1,
    DA_TV_COL_NAME,      name,
    DA_TV_COL_PATH,      e->path ? e->path : "",
    DA_TV_COL_PCT_LABEL, pct_label,
    DA_TV_COL_SIZE,      size_str,
    DA_TV_COL_ALLOC,     alloc_str,
    DA_TV_COL_ITEMS,     items_str,
    DA_TV_COL_FILES,     files_str,
    DA_TV_COL_FOLDERS,   folders_str,
    DA_TV_COL_MODIFIED,  mtime_str,
    DA_TV_COL_ATTRS,     attrs_str,
    DA_TV_COL_PCT_VAL,   pct_val,
    DA_TV_COL_IDX_ID,    (gint64)eid,
    DA_TV_COL_KIND,      (guint)e->kind,
    -1);

  /* For directories with children: add placeholder so the expander arrow appears. */
  if (e->first_child_id >= 0) {
    GtkTreeIter placeholder;
    gtk_tree_store_insert_with_values(app->tree_view_store, &placeholder, &iter, -1,
      DA_TV_COL_NAME,   "",
      DA_TV_COL_PATH,   "",
      DA_TV_COL_IDX_ID, DA_TV_LP_PLACEHOLDER,
      DA_TV_COL_KIND,   0u,
      -1);
  }
}

/** After populate, expand root row(s) on idle so GTK has applied store changes first. */
static gboolean da_tv_expand_top_level_idle(gpointer user_data) {
  AppState *app = (AppState *)user_data;
  if (app == NULL || app->tree_view == NULL || app->tree_view_store == NULL || !app->tree_view_populated) {
    return G_SOURCE_REMOVE;
  }
  GtkTreeView *tv = GTK_TREE_VIEW(app->tree_view);
  GtkTreeModel *m = GTK_TREE_MODEL(app->tree_view_store);
  GtkTreeIter iter;
  for (gboolean ok = gtk_tree_model_get_iter_first(m, &iter); ok; ok = gtk_tree_model_iter_next(m, &iter)) {
    GtkTreePath *p = gtk_tree_model_get_path(m, &iter);
    if (p == NULL) {
      continue;
    }
    /*
     * Programmatic gtk_tree_view_expand_row does not always emit row-expanded on every
     * platform/GTK build, so placeholder children would never be replaced. Run the same
     * work as the signal handler, then expand.
     */
    da_tree_view_on_row_expanded(tv, &iter, p, app);
    gtk_tree_view_expand_row(tv, p, FALSE);
    gtk_tree_path_free(p);
  }
  return G_SOURCE_REMOVE;
}

/* ---- GTask background build ---- */

typedef struct {
  scan_results_view_t v;       /* borrowed — lifetime managed by tv_held_scan */
  char               *scan_root; /* owned copy */
  DaTvEntry          *entries;   /* owned result; NULL until worker sets it */
  size_t              count;
} TvBuildData;

static void tv_build_data_free(TvBuildData *bd) {
  if (bd == NULL) { return; }
  g_free(bd->scan_root);
  free(bd->entries);
  g_free(bd);
}

/* Build DaTvEntry[] off the main thread. No GTK calls allowed here. */
static void tv_build_worker(GTask *task, gpointer source_object,
                             gpointer task_data, GCancellable *cancellable) {
  (void)source_object;
  TvBuildData *bd = (TvBuildData *)task_data;

  if (g_cancellable_is_cancelled(cancellable)) {
    g_task_return_error(task,
      g_error_new(G_IO_ERROR, G_IO_ERROR_CANCELLED, "tree-view build cancelled"));
    return;
  }

  scan_results_view_t v = bd->v;
  size_t n = v.count;
  if (v.nodes == NULL || n == 0) {
    bd->entries = NULL;
    bd->count   = 0;
    g_task_return_pointer(task, bd, NULL);
    return;
  }

  /* Step 1: pre-compute depth array (O(n·L)) then sort (O(n log n)). */
  size_t *sorted_idx = (size_t *)malloc(n * sizeof(size_t));
  uint8_t *depths    = (uint8_t *)malloc(n * sizeof(uint8_t));
  if (sorted_idx == NULL || depths == NULL) {
    free(sorted_idx);
    free(depths);
    g_task_return_error(task,
      g_error_new(G_IO_ERROR, G_IO_ERROR_NO_SPACE, "OOM in tree-view build"));
    return;
  }
  for (size_t i = 0; i < n; i++) {
    sorted_idx[i] = i;
    int d = count_separators(v.nodes[i].path);
    depths[i] = (uint8_t)(d > 255 ? 255 : d);
  }

  if (g_cancellable_is_cancelled(cancellable)) {
    free(sorted_idx);
    free(depths);
    g_task_return_error(task,
      g_error_new(G_IO_ERROR, G_IO_ERROR_CANCELLED, "tree-view build cancelled"));
    return;
  }

  tv_sort_nodes  = v.nodes;
  tv_sort_depths = depths;
  qsort(sorted_idx, n, sizeof(size_t), cmp_by_depth);
  tv_sort_nodes  = NULL;
  tv_sort_depths = NULL;
  free(depths);

  if (g_cancellable_is_cancelled(cancellable)) {
    free(sorted_idx);
    g_task_return_error(task,
      g_error_new(G_IO_ERROR, G_IO_ERROR_CANCELLED, "tree-view build cancelled"));
    return;
  }

  /* Step 2: allocate and initialise entry array. */
  DaTvEntry *entries = (DaTvEntry *)malloc(n * sizeof(DaTvEntry));
  if (entries == NULL) {
    free(sorted_idx);
    g_task_return_error(task,
      g_error_new(G_IO_ERROR, G_IO_ERROR_NO_SPACE, "OOM in tree-view build entries"));
    return;
  }
  for (size_t i = 0; i < n; i++) {
    if ((i & 0x1FFu) == 0u && g_cancellable_is_cancelled(cancellable)) {
      free(sorted_idx);
      free(entries);
      g_task_return_error(task,
        g_error_new_literal(G_IO_ERROR, G_IO_ERROR_CANCELLED, "tree-view build cancelled"));
      return;
    }
    entries[i].kind             = 0;
    entries[i].path             = NULL;
    entries[i].scan_nid         = SIZE_MAX;
    entries[i].mtime_unix_ns    = 0;
    entries[i].win32_attributes = 0;
    entries[i].size_bytes       = 0;
    entries[i].alloc_bytes      = 0;
    entries[i].file_count       = 0;
    entries[i].folder_count     = 0;
    entries[i].parent_id        = -1;
    entries[i].first_child_id   = -1;
    entries[i].next_sibling_id  = -1;
    entries[i].last_child_id    = -1;
  }

  /* Step 3: build tree links using a path→position hash table. */
  GHashTable *path_map = g_hash_table_new(g_str_hash, g_str_equal);

  for (size_t pos = 0; pos < n; pos++) {
    if ((pos & 0x1FFu) == 0u && g_cancellable_is_cancelled(cancellable)) {
      g_hash_table_destroy(path_map);
      free(sorted_idx);
      free(entries);
      g_task_return_error(task,
        g_error_new_literal(G_IO_ERROR, G_IO_ERROR_CANCELLED, "tree-view build cancelled"));
      return;
    }
    size_t ni = sorted_idx[pos];
    const file_node_t *node = &v.nodes[ni];
    DaTvEntry *e = &entries[pos];

    uint32_t kind = node->attributes & DISKATLAS_NODE_KIND_MASK;
    e->kind             = kind;
    e->path             = node->path;
    e->scan_nid         = ni;
    e->mtime_unix_ns    = node->mtime_unix_ns;
    e->win32_attributes = node->win32_attributes;

    if (kind == DISKATLAS_NODE_KIND_DIR) {
      e->size_bytes  = 0;
      e->alloc_bytes = 0;
      e->file_count  = 0;
      e->folder_count = 0;
    } else {
      e->size_bytes  = node->size_bytes;
      e->alloc_bytes = node->allocated_bytes;
      e->file_count  = 1;
      e->folder_count = 0;
    }

    if (node->path != NULL) {
      gchar *pp = compute_parent_path(node->path);
      if (pp != NULL) {
        gpointer pval = g_hash_table_lookup(path_map, pp);
        if (pval != NULL) {
          int32_t pid = (int32_t)((gintptr)pval - 1);
          e->parent_id = pid;
          if (entries[pid].last_child_id >= 0) {
            entries[entries[pid].last_child_id].next_sibling_id = (int32_t)pos;
          } else {
            entries[pid].first_child_id = (int32_t)pos;
          }
          entries[pid].last_child_id = (int32_t)pos;
        }
        g_free(pp);
      }
      g_hash_table_insert(path_map, (gpointer)node->path, (gpointer)(gintptr)(pos + 1));
    }
  }

  g_hash_table_destroy(path_map);
  free(sorted_idx);

  /* Step 4: bottom-up aggregation. */
  for (int32_t pos = (int32_t)n - 1; pos >= 0; pos--) {
    if ((pos & 0x3FF) == 0 && g_cancellable_is_cancelled(cancellable)) {
      free(entries);
      g_task_return_error(task,
        g_error_new_literal(G_IO_ERROR, G_IO_ERROR_CANCELLED, "tree-view build cancelled"));
      return;
    }
    DaTvEntry *e = &entries[pos];
    if (e->parent_id < 0) { continue; }
    DaTvEntry *parent = &entries[e->parent_id];
    parent->size_bytes  += e->size_bytes;
    parent->alloc_bytes += e->alloc_bytes;
    parent->file_count  += e->file_count;
    if (e->kind == DISKATLAS_NODE_KIND_DIR) {
      parent->folder_count += 1u + e->folder_count;
    }
  }

  /* Step 4b: synthetic root. */
  const char *scan_root_path = (bd->scan_root != NULL && bd->scan_root[0] != '\0')
                                ? bd->scan_root : "";
  {
    DaTvEntry *entries_new = (DaTvEntry *)realloc(entries, (n + 1) * sizeof(DaTvEntry));
    if (entries_new != NULL) {
      entries = entries_new;
      DaTvEntry *sr = &entries[n];
      sr->kind             = DISKATLAS_NODE_KIND_DIR;
      sr->path             = scan_root_path;
      sr->scan_nid         = SIZE_MAX;
      sr->mtime_unix_ns    = 0;
      sr->win32_attributes = 0;
      sr->size_bytes       = 0;
      sr->alloc_bytes      = 0;
      sr->file_count       = 0;
      sr->folder_count     = 0;
      sr->parent_id        = -1;
      sr->first_child_id   = -1;
      sr->next_sibling_id  = -1;
      sr->last_child_id    = -1;

      for (size_t i = 0; i < n; i++) {
        if (entries[i].parent_id != -1) { continue; }
        entries[i].parent_id = (int32_t)n;
        if (sr->last_child_id >= 0) {
          entries[sr->last_child_id].next_sibling_id = (int32_t)i;
        } else {
          sr->first_child_id = (int32_t)i;
        }
        sr->last_child_id = (int32_t)i;
        sr->size_bytes  += entries[i].size_bytes;
        sr->alloc_bytes += entries[i].alloc_bytes;
        sr->file_count  += entries[i].file_count;
        if (entries[i].kind == DISKATLAS_NODE_KIND_DIR) {
          sr->folder_count += 1u + entries[i].folder_count;
        }
      }
      n++;
    }
  }

  bd->entries = entries;
  bd->count   = n;
  g_task_return_pointer(task, bd, NULL);
}

/* Main-thread callback: receives TvBuildData from worker and does GTK work. */
static void tv_build_done(GObject *source_object, GAsyncResult *res, gpointer user_data) {
  (void)source_object;
  AppState *app = (AppState *)user_data;

  /* Release the held-scan reference.
   *
   * DaTvEntry.path borrows directly from the scan nodes, so the scan must
   * stay alive at least as long as the tree-view model.
   *
   * Two cases:
   *  a) No new scan started while the worker was running:
   *     tv_held_scan == app->scan.  app->scan still owns the data; we just
   *     clear our borrow reference and leave the scan alive.
   *  b) A new scan was started (start_scan set app->scan = NULL and handed
   *     ownership to tv_held_scan).  Nobody else holds the old scan, so we
   *     must free it here.
   */
  if (app->tv_held_scan != NULL) {
    if (app->tv_held_scan != app->scan) {
      scan_result_free(app->tv_held_scan);
    }
    app->tv_held_scan = NULL;
  }
  if (app->tv_build_cancel != NULL) {
    g_object_unref(app->tv_build_cancel);
    app->tv_build_cancel = NULL;
  }

  GError *err = NULL;
  TvBuildData *bd = (TvBuildData *)g_task_propagate_pointer(G_TASK(res), &err);
  if (err != NULL) {
    g_error_free(err);
    /* Cancelled or OOM — discard without updating the tree. */
    tv_build_data_free(bd);
    goto done;
  }
  if (bd == NULL) {
    goto done;
  }

  if (app->tree_view_store == NULL) {
    tv_build_data_free(bd);
    goto done;
  }

  /* Swap model. */
  if (app->tree_view_model != NULL) {
    free(app->tree_view_model->entries);
    g_free(app->tree_view_model->scan_root_str);
    free(app->tree_view_model);
    app->tree_view_model = NULL;
  }

  if (bd->entries == NULL || bd->count == 0) {
    tv_build_data_free(bd);
    goto done;
  }

  DaTreeViewModel *model = (DaTreeViewModel *)malloc(sizeof(DaTreeViewModel));
  if (model == NULL) {
    tv_build_data_free(bd);
    goto done;
  }
  model->entries       = bd->entries;
  model->count         = bd->count;
  model->scan_root_str = bd->scan_root; /* transfer ownership — synthetic root path points here */
  bd->entries          = NULL;          /* transferred */
  bd->count            = 0;
  bd->scan_root        = NULL;          /* transferred — prevent tv_build_data_free from freeing */
  tv_build_data_free(bd);

  app->tree_view_model = model;

  size_t n = model->count;
  DaTvEntry *entries = model->entries;

  /* Populate GTK store with root-level entries. */
  gtk_tree_store_clear(app->tree_view_store);
  for (size_t i = 0; i < n; i++) {
    if (entries[i].parent_id == -1) {
      da_tv_insert_entry(app, NULL, (int32_t)i);
    }
  }

  app->tree_view_populated = TRUE;

  if (app->tree_view != NULL) {
    g_idle_add(da_tv_expand_top_level_idle, app);
  }

done:
  app->tv_build_worker_pending = FALSE;
  if (app->defer_fs_monitor_phase_end) {
    app->defer_fs_monitor_phase_end = FALSE;
    da_fs_monitor_scan_phase_end(app);
  }
}

/* ---- Public: populate ---- */

gboolean da_tree_view_populate(AppState *app) {
  if (app == NULL || app->scan == NULL || app->tree_view_store == NULL) {
    return FALSE;
  }

  scan_results_view_t v = scan_get_results(app->scan);
  if (v.nodes == NULL || v.count == 0) {
    return FALSE;
  }

  /* Cancel any pending build first. */
  if (app->tv_build_cancel != NULL) {
    g_cancellable_cancel(app->tv_build_cancel);
    g_object_unref(app->tv_build_cancel);
    app->tv_build_cancel = NULL;
  }
  app->defer_fs_monitor_phase_end = FALSE;

  /* Keep the scan alive while the worker runs. */
  app->tv_held_scan = app->scan;

  TvBuildData *bd = g_new0(TvBuildData, 1);
  bd->v = v;
  {
    const gchar *root_src = "";
    if (app->scan_root_utf8 != NULL && app->scan_root_utf8[0] != '\0') {
      root_src = app->scan_root_utf8;
    } else if (app->csv_derived_root_utf8 != NULL && app->csv_derived_root_utf8[0] != '\0') {
      root_src = app->csv_derived_root_utf8;
    }
    bd->scan_root = g_strdup(root_src);
  }

  GCancellable *cancel = g_cancellable_new();
  app->tv_build_cancel = cancel;

  app->defer_fs_monitor_phase_end = TRUE;
  app->tv_build_worker_pending = TRUE;

  GTask *task = g_task_new(NULL, cancel, tv_build_done, app);
  g_task_set_task_data(task, bd, NULL);
  g_task_run_in_thread(task, tv_build_worker);
  g_object_unref(task);
  return TRUE;
}

/* ---- Public: clear ---- */

void da_tree_view_clear(AppState *app) {
  if (app == NULL) {
    return;
  }
  app->defer_fs_monitor_phase_end = FALSE;
  /* Cancel any background build; the callback will free tv_held_scan. */
  if (app->tv_build_cancel != NULL) {
    g_cancellable_cancel(app->tv_build_cancel);
    g_object_unref(app->tv_build_cancel);
    app->tv_build_cancel = NULL;
  }
  if (app->tree_view_store != NULL) {
    gtk_tree_store_clear(app->tree_view_store);
  }
  if (app->tree_view_model != NULL) {
    free(app->tree_view_model->entries);
    g_free(app->tree_view_model->scan_root_str);
    free(app->tree_view_model);
    app->tree_view_model = NULL;
  }
  app->tree_view_populated = FALSE;
}

static void da_tv_refresh_size_columns_branch(GtkTreeModel *model, GtkTreeIter *iter, AppState *app) {
  gint64 eid;
  gtk_tree_model_get(model, iter, DA_TV_COL_IDX_ID, &eid, -1);
  if (eid >= 0 && eid != DA_TV_LP_PLACEHOLDER && app->tree_view_model != NULL &&
      (gsize)eid < app->tree_view_model->count) {
    char pct_label[48], size_str[64], alloc_str[64];
    gint pct_val;
    da_tv_format_row_strings(app->tree_view_model, (int32_t)eid, pct_label, sizeof(pct_label), size_str,
                             sizeof(size_str), alloc_str, sizeof(alloc_str), &pct_val);
    gtk_tree_store_set(GTK_TREE_STORE(model), iter, DA_TV_COL_PCT_LABEL, pct_label, DA_TV_COL_SIZE, size_str,
                       DA_TV_COL_ALLOC, alloc_str, DA_TV_COL_PCT_VAL, pct_val, -1);
  }
  GtkTreeIter child;
  if (gtk_tree_model_iter_children(model, &child, iter)) {
    do {
      da_tv_refresh_size_columns_branch(model, &child, app);
    } while (gtk_tree_model_iter_next(model, &child));
  }
}

void da_tree_view_refresh_size_columns(AppState *app) {
  if (app == NULL || app->tree_view_store == NULL || app->tree_view_model == NULL || !app->tree_view_populated) {
    return;
  }
  GtkTreeModel *model = GTK_TREE_MODEL(app->tree_view_store);
  GtkTreeIter iter;
  for (gboolean ok = gtk_tree_model_get_iter_first(model, &iter); ok; ok = gtk_tree_model_iter_next(model, &iter)) {
    da_tv_refresh_size_columns_branch(model, &iter, app);
  }
}

void da_tree_view_model_sync_entry_paths_from_scan(AppState *app) {
  if (app == NULL || app->tree_view_model == NULL || app->scan == NULL) {
    return;
  }
  scan_results_view_t v = scan_get_results(app->scan);
  if (v.nodes == NULL) {
    return;
  }
  for (size_t i = 0; i < app->tree_view_model->count; i++) {
    DaTvEntry *e = &app->tree_view_model->entries[i];
    if (e->scan_nid != SIZE_MAX && e->scan_nid < v.count) {
      e->path = v.nodes[e->scan_nid].path;
    }
  }
}

static void da_tv_refresh_path_columns_branch(GtkTreeModel *model, GtkTreeIter *iter, AppState *app) {
  gint64 eid;
  gtk_tree_model_get(model, iter, DA_TV_COL_IDX_ID, &eid, -1);
  if (eid >= 0 && eid != DA_TV_LP_PLACEHOLDER && app->tree_view_model != NULL &&
      (gsize)eid < app->tree_view_model->count) {
    const DaTvEntry *e = &app->tree_view_model->entries[(int32_t)eid];
    const char *p = e->path != NULL ? e->path : "";
    gtk_tree_store_set(GTK_TREE_STORE(model), iter, DA_TV_COL_NAME, tv_display_name(p), DA_TV_COL_PATH, p, -1);
  }
  GtkTreeIter child;
  if (gtk_tree_model_iter_children(model, &child, iter)) {
    do {
      da_tv_refresh_path_columns_branch(model, &child, app);
    } while (gtk_tree_model_iter_next(model, &child));
  }
}

void da_tree_view_store_refresh_path_columns(AppState *app) {
  if (app == NULL || app->tree_view_store == NULL || app->tree_view_model == NULL || !app->tree_view_populated) {
    return;
  }
  GtkTreeModel *model = GTK_TREE_MODEL(app->tree_view_store);
  GtkTreeIter iter;
  for (gboolean ok = gtk_tree_model_get_iter_first(model, &iter); ok; ok = gtk_tree_model_iter_next(model, &iter)) {
    da_tv_refresh_path_columns_branch(model, &iter, app);
  }
}

/* ---- Public: row-expanded (lazy load children) ---- */

void da_tree_view_on_row_expanded(GtkTreeView *tv, GtkTreeIter *iter,
                                  GtkTreePath *path, gpointer user_data) {
  (void)tv;
  (void)path;
  AppState *app = (AppState *)user_data;
  if (app == NULL || app->tree_view_model == NULL || app->tree_view_store == NULL) {
    return;
  }

  GtkTreeModel *model = GTK_TREE_MODEL(app->tree_view_store);

  /* Check first child: if it's a placeholder, replace with real children. */
  GtkTreeIter child;
  if (!gtk_tree_model_iter_children(model, &child, iter)) {
    return;
  }

  gint64 child_id = 0;
  gtk_tree_model_get(model, &child, DA_TV_COL_IDX_ID, &child_id, -1);
  if (child_id != DA_TV_LP_PLACEHOLDER) {
    /* Already expanded — nothing to do. */
    return;
  }

  gint64 eid = 0;
  gtk_tree_model_get(model, iter, DA_TV_COL_IDX_ID, &eid, -1);
  if (eid < 0 || (size_t)eid >= app->tree_view_model->count) {
    return;
  }

  const DaTvEntry *e = &app->tree_view_model->entries[(int32_t)eid];

  /* Remove placeholder before inserting real rows. */
  gtk_tree_store_remove(app->tree_view_store, &child);

  /* Insert each child entry (with its own placeholder if it has children). */
  int32_t cid = e->first_child_id;
  while (cid >= 0 && (size_t)cid < app->tree_view_model->count) {
    da_tv_insert_entry(app, iter, cid);
    cid = app->tree_view_model->entries[cid].next_sibling_id;
  }
}

gboolean da_tree_view_model_borrow_path_for_entry_id(const DaTreeViewModel *m, gint64 entry_id,
                                                     const char **out_path) {
  if (out_path != NULL) {
    *out_path = NULL;
  }
  if (m == NULL || out_path == NULL) {
    return FALSE;
  }
  if (entry_id < 0 || (size_t)entry_id >= m->count) {
    return FALSE;
  }
  *out_path = m->entries[(size_t)entry_id].path;
  return TRUE;
}

gboolean da_tv_entry_get_stats(const DaTreeViewModel *m, gint64 entry_id,
                               uint64_t *out_size, uint64_t *out_alloc,
                               uint64_t *out_file_count) {
  if (m == NULL || entry_id < 0 || (size_t)entry_id >= m->count) {
    return FALSE;
  }
  const DaTvEntry *e = &m->entries[(size_t)entry_id];
  if (out_size)       *out_size       = e->size_bytes;
  if (out_alloc)      *out_alloc      = e->alloc_bytes;
  if (out_file_count) *out_file_count = e->file_count;
  return TRUE;
}
