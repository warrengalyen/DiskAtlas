#ifndef FLAT_LIST_MODEL_H
#define FLAT_LIST_MODEL_H

#include <gtk/gtk.h>
#include <stddef.h>

/* AppState is defined in app_state.h; forward-declare to avoid a circular include. */
typedef struct AppState AppState;

#define FLAT_LIST_TYPE_MODEL (flat_list_model_get_type())
G_DECLARE_FINAL_TYPE(FlatListModel, flat_list_model, FLAT_LIST, MODEL, GObject)

/** Create a new FlatListModel bound to the given AppState (borrowed pointer). */
FlatListModel *flat_list_model_new(AppState *app);

/**
 * Replace the current row set with a copy of the given indices array.
 * GTK is notified immediately; no timer or incremental insert loop is needed.
 * Pass indices == NULL or count == 0 to clear all rows.
 */
void flat_list_model_set_indices(FlatListModel *m, const size_t *indices, size_t count);

/**
 * Emit row-changed for every row — use when displayed values may have
 * changed without the index set changing (e.g. volume_pct_denominator_bytes
 * was updated).
 */
void flat_list_model_invalidate(FlatListModel *m);

#endif /* FLAT_LIST_MODEL_H */
