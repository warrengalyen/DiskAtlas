#ifndef TREEMAP_WIDGET_H
#define TREEMAP_WIDGET_H

#include <gtk/gtk.h>

#include "diskatlas.h"

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

#endif
