#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include <glib.h>
#include <gtk/gtk.h>
#include <gdk/gdk.h>
#include <cairo.h>

#include "file_type_view.h"
#include "dm_mime_db.h"
#include "dm_treemap_colors.h"
#include "diskatlas.h"
#include "format_text.h"
#include "da_cell_renderer_progress.h"
#include "shell_icon.h"

/* -------------------------------------------------------------------------
 * Internal helpers
 * ---------------------------------------------------------------------- */

/** Extract the last path component after '/' or '\'. */
static const char *ft_get_basename(const char *path) {
  if (path == NULL) return NULL;
  const char *p = strrchr(path, '/');
  if (p != NULL) path = p + 1;
#if defined(_WIN32) || defined(G_OS_WIN32)
  const char *q = strrchr(path, '\\');
  if (q != NULL) path = q + 1;
#endif
  return path;
}

/** Return pointer to the last '.' that starts an extension; NULL for leading dots or no dot. */
static const char *ft_get_extension(const char *name) {
  if (name == NULL) return NULL;
  const char *dot = strrchr(name, '.');
  if (dot == NULL || dot == name) return NULL;
  return dot;
}

/* ---- Per-extension accumulator ---------------------------------------- */

typedef struct {
  gchar    *key;          /* Owned: lowercase ".ext" or exact basename */
  uint32_t  color_rgba;   /* From the first node seen for this key */
  uint8_t   mime_cat_id;
  uint64_t  size_bytes;
  uint64_t  alloc_bytes;
  uint64_t  file_count;
} DaFtEntry;

static void da_ft_entry_free(gpointer p) {
  if (p == NULL) return;
  DaFtEntry *e = (DaFtEntry *)p;
  g_free(e->key);
  g_free(e);
}

/* ---- Color swatch pixbuf generation ----------------------------------- */

#define DA_FT_SWATCH_W 16  /* rendered color area width (px) */
#define DA_FT_SWATCH_H 18  /* rendered color area height (px) */

/**
 * Create a GdkPixbuf of DA_FT_SWATCH_W × DA_FT_SWATCH_H rendering the given
 * color with the same radial gradient the treemap uses (when enabled), or a
 * solid fill (when disabled).  Caller owns the returned pixbuf (unref when done).
 */
static GdkPixbuf *ft_make_swatch_pixbuf(uint32_t color_rgba,
                                         const DmTreemapStyle *style) {
  cairo_surface_t *surf = cairo_image_surface_create(CAIRO_FORMAT_RGB24,
                                                     DA_FT_SWATCH_W,
                                                     DA_FT_SWATCH_H);
  cairo_t *cr = cairo_create(surf);

  if (style != NULL && style->enable_tile_gradients) {
    file_node_t syn;
    memset(&syn, 0, sizeof(syn));
    syn.struct_version  = DISKATLAS_FILE_NODE_STRUCT_VERSION;
    syn.mime_color_rgba = color_rgba;
    double shadow = style->gradient_strength *
                    (DM_TREEMAP_DEFAULT_SHADOW_STRENGTH /
                     DM_TREEMAP_DEFAULT_GRADIENT_STRENGTH);
    dm_file_node_compute_gradient_colors(&syn, style->gradient_strength, shadow);
    dm_treemap_draw_gradient_tile(cr, &syn, 0.0, 0.0,
                                  DA_FT_SWATCH_W, DA_FT_SWATCH_H, style);
  } else {
    double rf = ((color_rgba >> 24) & 0xFFu) / 255.0;
    double gf = ((color_rgba >> 16) & 0xFFu) / 255.0;
    double bf = ((color_rgba >>  8) & 0xFFu) / 255.0;
    cairo_set_source_rgb(cr, rf, gf, bf);
    cairo_paint(cr);
  }

  cairo_destroy(cr);
  GdkPixbuf *pb = gdk_pixbuf_get_from_surface(surf, 0, 0,
                                               DA_FT_SWATCH_W, DA_FT_SWATCH_H);
  cairo_surface_destroy(surf);
  return pb;
}

static void ft_pct_cell_data(GtkTreeViewColumn *col, GtkCellRenderer *cell,
                             GtkTreeModel *model, GtkTreeIter *iter,
                             gpointer user_data) {
  (void)user_data;
  gint pv = 0;
  gchar *txt = NULL;
  gtk_tree_model_get(model, iter, DA_FT_COL_PCT_VAL, &pv, DA_FT_COL_PCT_LBL, &txt, -1);
  gint v = (pv > 100) ? 100 : (pv < 0 ? 0 : pv);

  gint col_w = gtk_tree_view_column_get_width(col);
  if (col_w <= 1) col_w = gtk_tree_view_column_get_fixed_width(col);
  if (col_w > 4) {
    gtk_cell_renderer_set_fixed_size(cell, col_w - 4, -1);
  } else {
    gtk_cell_renderer_set_fixed_size(cell, -1, -1);
  }

  g_object_set(GTK_CELL_RENDERER_PROGRESS(cell),
               "value", v,
               "text", txt != NULL ? txt : "",
               "text-xalign", 0.98f,
               NULL);
  g_free(txt);
}

/* ---- Column builder helpers ------------------------------------------- */

static void append_ft_text_column(GtkTreeView *tv, const char *title,
                                  int model_col, int sort_col,
                                  int width_px, int min_width_px,
                                  gfloat xalign) {
  GtkCellRenderer *r = gtk_cell_renderer_text_new();
  g_object_set(r, "ellipsize", PANGO_ELLIPSIZE_END, "xalign", xalign, NULL);
  GtkTreeViewColumn *c = gtk_tree_view_column_new_with_attributes(title, r, "text", model_col, NULL);
  gtk_tree_view_column_set_alignment(c, xalign);
  gtk_tree_view_column_set_resizable(c, TRUE);
  gtk_tree_view_column_set_sizing(c, GTK_TREE_VIEW_COLUMN_FIXED);
  gtk_tree_view_column_set_min_width(c, min_width_px);
  gtk_tree_view_column_set_fixed_width(c, width_px);
  gtk_tree_view_column_set_sort_column_id(c, sort_col);
  gtk_tree_view_append_column(tv, c);
}

static void append_ft_pct_column(GtkTreeView *tv, const char *title,
                                 int sort_col, int width_px, int min_width_px) {
  GtkCellRenderer *r = da_cell_renderer_progress_new();
  g_object_set(r, "xpad", 0, "ypad", 0, "xalign", 0.0f, NULL);
  GtkTreeViewColumn *c = gtk_tree_view_column_new();
  gtk_tree_view_column_set_title(c, title);
  gtk_tree_view_column_pack_start(c, r, TRUE);
  gtk_tree_view_column_set_cell_data_func(c, r, ft_pct_cell_data, NULL, NULL);
  gtk_tree_view_column_set_alignment(c, 1.0f);
  gtk_tree_view_column_set_resizable(c, TRUE);
  gtk_tree_view_column_set_sizing(c, GTK_TREE_VIEW_COLUMN_FIXED);
  gtk_tree_view_column_set_min_width(c, min_width_px);
  gtk_tree_view_column_set_fixed_width(c, width_px);
  gtk_tree_view_column_set_sort_column_id(c, sort_col);
  gtk_tree_view_append_column(tv, c);
}

/* ---- Comparison for initial GPtrArray sort ----------------------------- */

static int cmp_ft_entry_size_desc(gconstpointer a, gconstpointer b) {
  const DaFtEntry *ea = *(const DaFtEntry **)a;
  const DaFtEntry *eb = *(const DaFtEntry **)b;
  if (ea->size_bytes > eb->size_bytes) return -1;
  if (ea->size_bytes < eb->size_bytes) return  1;
  return 0;
}

/* ---- Public API -------------------------------------------------------- */

void da_file_type_view_setup(AppState *app) {
  if (app->file_type_tree == NULL) return;

  GtkTreeView *tv = GTK_TREE_VIEW(app->file_type_tree);

  GtkListStore *store = gtk_list_store_new(DA_FT_N_COLS,
    G_TYPE_UINT,    /* DA_FT_COL_COLOR     */
    G_TYPE_STRING,  /* DA_FT_COL_EXT       */
    G_TYPE_STRING,  /* DA_FT_COL_TYPE      */
    G_TYPE_INT,     /* DA_FT_COL_PCT_VAL   */
    G_TYPE_STRING,  /* DA_FT_COL_PCT_LBL   */
    G_TYPE_STRING,  /* DA_FT_COL_SIZE      */
    G_TYPE_STRING,  /* DA_FT_COL_ALLOC     */
    G_TYPE_STRING,  /* DA_FT_COL_FILES     */
    G_TYPE_UINT64,  /* DA_FT_COL_SIZE_RAW  */
    G_TYPE_UINT64,  /* DA_FT_COL_ALLOC_RAW */
    G_TYPE_UINT64,  /* DA_FT_COL_FILES_RAW */
    G_TYPE_OBJECT,  /* DA_FT_COL_COLOR_PB  */
    G_TYPE_OBJECT   /* DA_FT_COL_ICON_PB   */
  );

  gtk_tree_sortable_set_sort_column_id(GTK_TREE_SORTABLE(store),
                                       DA_FT_COL_SIZE_RAW,
                                       GTK_SORT_DESCENDING);

  gtk_tree_view_set_model(tv, GTK_TREE_MODEL(store));
  g_object_unref(store);

  gtk_tree_view_set_fixed_height_mode(tv, TRUE);
  gtk_tree_selection_set_mode(gtk_tree_view_get_selection(tv), GTK_SELECTION_SINGLE);

  /* Color swatch column — GtkCellRendererPixbuf, no sort, fixed width. */
  {
    GtkCellRenderer *r = gtk_cell_renderer_pixbuf_new();
    g_object_set(r, "xpad", 0, "ypad", 0, "xalign", 0.5f, NULL);
    GtkTreeViewColumn *c = gtk_tree_view_column_new();
    gtk_tree_view_column_set_title(c, "");
    gtk_tree_view_column_pack_start(c, r, TRUE);
    gtk_tree_view_column_add_attribute(c, r, "pixbuf", DA_FT_COL_COLOR_PB);
    gtk_tree_view_column_set_sizing(c, GTK_TREE_VIEW_COLUMN_FIXED);
    gtk_tree_view_column_set_fixed_width(c, 25);
    gtk_tree_view_column_set_min_width(c, 25);
    gtk_tree_view_column_set_resizable(c, FALSE);
    gtk_tree_view_append_column(tv, c);
  }

  /* Extension column: OS file-type icon + extension text, sortable by text. */
  {
    GtkTreeViewColumn *c = gtk_tree_view_column_new();
    gtk_tree_view_column_set_title(c, "Extension");

    GtkCellRenderer *icon_r = gtk_cell_renderer_pixbuf_new();
    g_object_set(icon_r, "xpad", 1, "ypad", 0, NULL);
    gtk_tree_view_column_pack_start(c, icon_r, FALSE);
    gtk_tree_view_column_add_attribute(c, icon_r, "pixbuf", DA_FT_COL_ICON_PB);

    GtkCellRenderer *text_r = gtk_cell_renderer_text_new();
    g_object_set(text_r, "ellipsize", PANGO_ELLIPSIZE_END, "xalign", 0.0f, NULL);
    gtk_tree_view_column_pack_start(c, text_r, TRUE);
    gtk_tree_view_column_add_attribute(c, text_r, "text", DA_FT_COL_EXT);

    gtk_tree_view_column_set_alignment(c, 0.0f);
    gtk_tree_view_column_set_resizable(c, TRUE);
    gtk_tree_view_column_set_sizing(c, GTK_TREE_VIEW_COLUMN_FIXED);
    gtk_tree_view_column_set_min_width(c, 60);
    gtk_tree_view_column_set_fixed_width(c, 110);
    gtk_tree_view_column_set_sort_column_id(c, DA_FT_COL_EXT);
    gtk_tree_view_append_column(tv, c);
  }
  append_ft_text_column(tv, "File Type",  DA_FT_COL_TYPE,  DA_FT_COL_TYPE,      160, 80, 0.0f);
  append_ft_pct_column (tv, "Percent",    DA_FT_COL_SIZE_RAW, 110, 88);
  append_ft_text_column(tv, "Size",       DA_FT_COL_SIZE,  DA_FT_COL_SIZE_RAW,  100,  72, 1.0f);
  append_ft_text_column(tv, "Allocated",  DA_FT_COL_ALLOC, DA_FT_COL_ALLOC_RAW, 100,  72, 1.0f);
  append_ft_text_column(tv, "Files",      DA_FT_COL_FILES, DA_FT_COL_FILES_RAW,  80,  56, 1.0f);
}

void da_file_type_view_clear(AppState *app) {
  if (app->file_type_tree == NULL) return;
  GtkTreeModel *model = gtk_tree_view_get_model(GTK_TREE_VIEW(app->file_type_tree));
  if (model != NULL && GTK_IS_LIST_STORE(model)) {
    gtk_list_store_clear(GTK_LIST_STORE(model));
  }
}

void da_file_type_view_populate(AppState *app) {
  if (app->file_type_tree == NULL) return;
  if (app->scan == NULL) {
    da_file_type_view_clear(app);
    return;
  }

  scan_results_view_t v = scan_get_results(app->scan);
  if (v.nodes == NULL || v.count == 0) {
    da_file_type_view_clear(app);
    return;
  }

  /* Build extension → DaFtEntry accumulator table. */
  GHashTable *ht = g_hash_table_new_full(g_str_hash, g_str_equal, NULL, da_ft_entry_free);

  for (size_t i = 0; i < v.count; i++) {
    const file_node_t *fn = &v.nodes[i];
    uint32_t kind = fn->attributes & DISKATLAS_NODE_KIND_MASK;
    if (kind == DISKATLAS_NODE_KIND_DIR) continue;

    const char *path = fn->path;
    if (path == NULL) continue;

    const char *base = ft_get_basename(path);
    if (base == NULL || *base == '\0') continue;

    const char *ext = ft_get_extension(base);
    gchar *key;
    if (ext != NULL) {
      key = g_ascii_strdown(ext, -1);  /* e.g. ".mp3" */
    } else {
      key = g_strdup(base);  /* extension-less, e.g. "Makefile" */
    }

    DaFtEntry *entry = (DaFtEntry *)g_hash_table_lookup(ht, key);
    if (entry == NULL) {
      entry = g_new0(DaFtEntry, 1);
      entry->key         = key;  /* ownership transferred to entry */
      entry->color_rgba  = fn->mime_color_rgba;
      entry->mime_cat_id = fn->mime_category_id;
      g_hash_table_insert(ht, key, entry);
    } else {
      g_free(key);  /* already in table */
    }

    entry->size_bytes  += fn->size_bytes;
    entry->alloc_bytes += fn->allocated_bytes;
    entry->file_count  += 1;
  }

  /* Convert to sorted array. */
  GPtrArray *arr = g_ptr_array_new();
  GHashTableIter iter;
  gpointer val;
  g_hash_table_iter_init(&iter, ht);
  while (g_hash_table_iter_next(&iter, NULL, &val)) {
    g_ptr_array_add(arr, val);
  }
  g_ptr_array_sort(arr, cmp_ft_entry_size_desc);

  /* Compute percent denominator. */
  uint64_t denom = app->volume_pct_denominator_bytes;
  if (denom == 0) {
    for (guint i = 0; i < arr->len; i++) {
      denom += ((DaFtEntry *)arr->pdata[i])->size_bytes;
    }
  }

  /* Populate the list store. */
  GtkTreeModel *model = gtk_tree_view_get_model(GTK_TREE_VIEW(app->file_type_tree));
  if (model == NULL || !GTK_IS_LIST_STORE(model)) {
    g_ptr_array_free(arr, FALSE);
    g_hash_table_destroy(ht);
    return;
  }
  GtkListStore *store = GTK_LIST_STORE(model);
  gtk_list_store_clear(store);

  char size_buf[64], alloc_buf[64], pct_buf[32], files_buf[32];

  for (guint i = 0; i < arr->len; i++) {
    DaFtEntry *e = (DaFtEntry *)arr->pdata[i];

    da_format_bytes(e->size_bytes,  size_buf,  sizeof(size_buf));
    da_format_bytes(e->alloc_bytes, alloc_buf, sizeof(alloc_buf));
    da_format_pct_progress_label(e->size_bytes, denom, pct_buf, sizeof(pct_buf));
    da_format_uint64_locale(e->file_count, files_buf, sizeof(files_buf));

    gint pct_val = 0;
    if (denom > 0) {
      double pf = (double)e->size_bytes / (double)denom * 100.0;
      pct_val = (gint)(pf > 100.0 ? 100 : pf < 0.0 ? 0 : pf);
    }

    /* OS file-type description (e.g. "MP3 audio file") and icon. */
    gchar *type_desc = da_shell_description_for_extension(e->key);
    GdkPixbuf *icon_pb = da_shell_icon_for_extension(e->key, 16);

    /* Use fallback gray when color is unset (unclassified). */
    guint color = e->color_rgba;
    if (color == 0) color = DISKATLAS_MIME_COLOR_FALLBACK;

    GdkPixbuf *pb = ft_make_swatch_pixbuf((uint32_t)color, &app->treemap_style);

    GtkTreeIter row;
    gtk_list_store_append(store, &row);
    gtk_list_store_set(store, &row,
      DA_FT_COL_COLOR,     color,
      DA_FT_COL_EXT,       e->key,
      DA_FT_COL_TYPE,      type_desc != NULL ? type_desc : "",
      DA_FT_COL_PCT_VAL,   pct_val,
      DA_FT_COL_PCT_LBL,   pct_buf,
      DA_FT_COL_SIZE,      size_buf,
      DA_FT_COL_ALLOC,     alloc_buf,
      DA_FT_COL_FILES,     files_buf,
      DA_FT_COL_SIZE_RAW,  (guint64)e->size_bytes,
      DA_FT_COL_ALLOC_RAW, (guint64)e->alloc_bytes,
      DA_FT_COL_FILES_RAW, (guint64)e->file_count,
      DA_FT_COL_COLOR_PB,  pb,
      DA_FT_COL_ICON_PB,   icon_pb,
      -1);

    if (pb != NULL) g_object_unref(pb);
    if (icon_pb != NULL) g_object_unref(icon_pb);
    g_free(type_desc);
  }

  g_ptr_array_free(arr, FALSE);  /* entries still owned by ht */
  g_hash_table_destroy(ht);
}
