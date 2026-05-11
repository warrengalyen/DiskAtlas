#include <gtk/gtk.h>

#include "da_cell_renderer_progress.h"

/**
 * GtkCellRendererProgress does not apply GtkCellRendererState to the tree widget's style context,
 * so layout/text ignore selection/prelight. Match GtkCellArea focus rendering by syncing context state.
 *
 * Additionally exposes a "strikethrough" boolean property: when TRUE, a horizontal red line is
 * drawn through the centre of the cell after the normal progress bar render (used for deleted items).
 */

enum {
  PROP_0,
  PROP_STRIKETHROUGH,
  N_PROPS
};

struct _DaCellRendererProgress {
  GtkCellRendererProgress parent_instance;
  gboolean strikethrough;
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

  DaCellRendererProgress *self = DA_CELL_RENDERER_PROGRESS(cell);
  if (self->strikethrough && cell_area != NULL && cell_area->width > 0) {
    gchar  *text       = NULL;
    gfloat  text_xalign = 0.5f;
    g_object_get(cell, "text", &text, "text-xalign", &text_xalign, NULL);

    if (text != NULL && text[0] != '\0') {
      PangoLayout *layout = gtk_widget_create_pango_layout(widget, text);
      int tw_pango = 0, th_pango = 0;
      pango_layout_get_pixel_size(layout, &tw_pango, &th_pango);
      g_object_unref(layout);

      gdouble text_w  = (gdouble)tw_pango;
      gdouble cell_w  = (gdouble)cell_area->width;
      gdouble x_start = cell_area->x + (cell_w - text_w) * (gdouble)text_xalign;
      if (x_start < cell_area->x) { x_start = cell_area->x; }
      gdouble x_end = x_start + text_w;
      if (x_end > cell_area->x + cell_w) { x_end = cell_area->x + cell_w; }

      gdouble mid_y = cell_area->y + cell_area->height / 2.0;

      cairo_save(cr);
      cairo_set_source_rgb(cr, 0.8, 0.0, 0.0);
      cairo_set_line_width(cr, 1.5);
      cairo_move_to(cr, x_start, mid_y);
      cairo_line_to(cr, x_end, mid_y);
      cairo_stroke(cr);
      cairo_restore(cr);
    }
    g_free(text);
  }
}

static void da_cell_renderer_progress_get_property(GObject *object, guint prop_id,
                                                    GValue *value, GParamSpec *pspec) {
  DaCellRendererProgress *self = DA_CELL_RENDERER_PROGRESS(object);
  switch (prop_id) {
    case PROP_STRIKETHROUGH:
      g_value_set_boolean(value, self->strikethrough);
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID(object, prop_id, pspec);
      break;
  }
}

static void da_cell_renderer_progress_set_property(GObject *object, guint prop_id,
                                                    const GValue *value, GParamSpec *pspec) {
  DaCellRendererProgress *self = DA_CELL_RENDERER_PROGRESS(object);
  switch (prop_id) {
    case PROP_STRIKETHROUGH:
      self->strikethrough = g_value_get_boolean(value);
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID(object, prop_id, pspec);
      break;
  }
}

static void da_cell_renderer_progress_class_init(DaCellRendererProgressClass *klass) {
  GObjectClass        *obj_class  = G_OBJECT_CLASS(klass);
  GtkCellRendererClass *cell_class = GTK_CELL_RENDERER_CLASS(klass);

  obj_class->get_property = da_cell_renderer_progress_get_property;
  obj_class->set_property = da_cell_renderer_progress_set_property;
  cell_class->render      = da_cell_renderer_progress_render;

  g_object_class_install_property(obj_class, PROP_STRIKETHROUGH,
    g_param_spec_boolean("strikethrough", "Strikethrough",
                         "Draw a horizontal line through the progress bar (deleted item indicator)",
                         FALSE,
                         G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));
}

static void da_cell_renderer_progress_init(DaCellRendererProgress *self) {
  self->strikethrough = FALSE;
}

GtkCellRenderer *da_cell_renderer_progress_new(void) {
  return g_object_new(DA_TYPE_CELL_RENDERER_PROGRESS, NULL);
}
