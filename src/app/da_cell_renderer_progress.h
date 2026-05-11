#ifndef DA_CELL_RENDERER_PROGRESS_H
#define DA_CELL_RENDERER_PROGRESS_H

#include <gtk/gtk.h>

G_BEGIN_DECLS

#define DA_TYPE_CELL_RENDERER_PROGRESS (da_cell_renderer_progress_get_type ())
#define DA_CELL_RENDERER_PROGRESS(obj) \
  (G_TYPE_CHECK_INSTANCE_CAST((obj), DA_TYPE_CELL_RENDERER_PROGRESS, DaCellRendererProgress))
#define DA_IS_CELL_RENDERER_PROGRESS(obj) \
  (G_TYPE_CHECK_INSTANCE_TYPE((obj), DA_TYPE_CELL_RENDERER_PROGRESS))

typedef struct _DaCellRendererProgress DaCellRendererProgress;
typedef struct _DaCellRendererProgressClass DaCellRendererProgressClass;

GType            da_cell_renderer_progress_get_type(void);
GtkCellRenderer *da_cell_renderer_progress_new(void);

G_END_DECLS

#endif
