#include <gtk/gtk.h>

#include "da_cell_renderer_progress.h"

/**
 * GtkCellRendererProgress does not apply GtkCellRendererState to the tree widget's style context,
 * so layout/text ignore selection/prelight. Match GtkCellArea focus rendering by syncing context state.
 */
struct _DaCellRendererProgress {
  GtkCellRendererProgress parent_instance;
};

struct _DaCellRendererProgressClass {
  GtkCellRendererProgressClass parent_class;
};

G_DEFINE_TYPE(DaCellRendererProgress, da_cell_renderer_progress, GTK_TYPE_CELL_RENDERER_PROGRESS)

static void da_cell_renderer_progress_render(GtkCellRenderer *cell, cairo_t *cr, GtkWidget *widget,
                                             const GdkRectangle *background_area,
                                             const GdkRectangle *cell_area, GtkCellRendererState flags) {
  GtkStyleContext *ctx = gtk_widget_get_style_context(widget);
  gtk_style_context_save(ctx);
  gtk_style_context_set_state(ctx, gtk_cell_renderer_get_state(cell, widget, flags));
  GTK_CELL_RENDERER_CLASS(da_cell_renderer_progress_parent_class)
      ->render(cell, cr, widget, background_area, cell_area, flags);
  gtk_style_context_restore(ctx);
}

static void da_cell_renderer_progress_class_init(DaCellRendererProgressClass *klass) {
  GtkCellRendererClass *cell_class = GTK_CELL_RENDERER_CLASS(klass);
  cell_class->render = da_cell_renderer_progress_render;
}

static void da_cell_renderer_progress_init(DaCellRendererProgress *self) {
  (void)self;
}

GtkCellRenderer *da_cell_renderer_progress_new(void) {
  return g_object_new(DA_TYPE_CELL_RENDERER_PROGRESS, NULL);
}
