#ifndef TREEMAP_WIDGET_H
#define TREEMAP_WIDGET_H

#include <gtk/gtk.h>

#include "diskatlas.h"
#include "dm_treemap_colors.h"

#define TREEMAP_TYPE_WIDGET (treemap_widget_get_type())
G_DECLARE_FINAL_TYPE(TreemapWidget, treemap_widget, TREEMAP, WIDGET, GtkDrawingArea)

/** One leaf tile (file or empty directory) packed with zero gap. */
typedef struct {
  double x, y, w, h;
  size_t node_index;
} treemap_rect_t;

GtkWidget *treemap_widget_new(void);

/**
 * Rebuilds directory tree from paths under @p root_utf8, runs slice-and-dice layout into @p rects,
 * then queues redraw. @p nodes is borrowed until the next set_data or widget destroy (same lifetime
 * as scan_results_view_t from the scan).
 */
void treemap_widget_set_data(TreemapWidget *widget, const char *root_utf8, const file_node_t *nodes,
                             size_t count);
/** Rebuild layout; when @p view_hidden is FALSE, omit hidden/recycle-bin nodes from the treemap. */
void treemap_widget_set_data_filtered(TreemapWidget *widget, const char *root_utf8,
                                      const file_node_t *nodes, size_t count, gboolean view_hidden);

/** Full treemap appearance (radial lighting, borders, thresholds). */
void treemap_widget_set_style(TreemapWidget *w, const DmTreemapStyle *s);

/** Toggle only `enable_tile_gradients` (same as clearing `enable_tile_gradients` in style). */
void treemap_widget_set_gradient_fill(TreemapWidget *w, gboolean gradient);

void treemap_widget_set_hover_callback(TreemapWidget *w,
                                       void (*cb)(GtkWidget *widget, gint64 scan_index, gpointer data),
                                       gpointer data);

void treemap_widget_set_selected_callback(TreemapWidget *w,
                                          void (*cb)(GtkWidget *widget, gint64 scan_index, gpointer data),
                                          gpointer data);

/**
 * Programmatically replace the treemap's selection with the single tile whose
 * node_index == scan_index, clearing any prior multi-selection.
 * Passing -1 clears all selections without selecting anything.
 */
void treemap_widget_set_selection_by_scan_index(TreemapWidget *w, gint64 scan_index);

/**
 * Add the tile whose node_index == scan_index to the current selection without
 * clearing existing selections.  No-op if scan_index is < 0 or not found.
 */
void treemap_widget_add_to_selection_by_scan_index(TreemapWidget *w, gint64 scan_index);

/** Append unique scan node indices for selected rects/dir headers (no-op if layout not ready). */
void treemap_widget_append_selected_scan_indices(TreemapWidget *w, GArray *out_nids);
/** Returns TRUE when at least one tile is currently selected. */
gboolean treemap_widget_has_selection(TreemapWidget *w);

/** Sentinel scan_index value delivered to the hover callback when the cursor enters the free-space tile. */
#define TREEMAP_SCAN_INDEX_FREE_SPACE ((gint64)-3)

/**
 * Configure free-space tile display.  When @p show is TRUE and @p free_bytes > 0, the layout
 * reserves a proportional strip for a "Free Space" tile drawn alongside the data tiles.
 * @p root_utf8 is the volume/folder label shown on the tile and in the hover status bar
 * (e.g. "D:\\" or "D:\\Users\\foo"); may be NULL.
 * Call after treemap_widget_set_data or whenever the volume space changes.
 */
void treemap_widget_set_free_space(TreemapWidget *w, gboolean show,
                                   uint64_t free_bytes, uint64_t used_bytes,
                                   const char *root_utf8);

/**
 * Toggle label rendering (file names + directory header text).  When @p show is FALSE only
 * color tiles are drawn — no text — matching WizTree's label-off mode.  Default: TRUE.
 */
void treemap_widget_set_show_labels(TreemapWidget *w, gboolean show);

/**
 * Register a callback fired on double-click of a tile (zoom-in gesture).
 * @p cb receives the scan_index of the clicked tile (-1 if empty area).
 */
void treemap_widget_set_zoom_callback(TreemapWidget *w,
                                      void (*cb)(GtkWidget *widget, gint64 scan_index, gpointer data),
                                      gpointer data);

/**
 * Render the treemap to a PNG file at the requested pixel dimensions.
 * Tile multi-selection outlines are omitted from the export (the on-screen selection is unchanged).
 * @p show_free_space / @p free_bytes / @p used_bytes override the widget's current free-space
 * state for the exported image only; the live widget state is fully restored afterwards.
 * Returns TRUE on success; FALSE if the widget has no layout or the file cannot be written.
 */
gboolean treemap_widget_export_png(TreemapWidget *w, const char *output_path,
                                   int width, int height,
                                   gboolean grayscale,
                                   gboolean show_free_space,
                                   uint64_t free_bytes,
                                   uint64_t used_bytes);

#endif
