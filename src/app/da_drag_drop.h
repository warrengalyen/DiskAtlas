#ifndef DA_DRAG_DROP_H
#define DA_DRAG_DROP_H

#include "app_state.h"

/**
 * Enable outbound drag-and-drop on column-0 of both file_view_tree and
 * tree_view_tree, if app->general_enable_drag_drop is TRUE.
 *
 * Behaviour:
 *   - Dragging a row (or the current multi-selection) onto the OS file
 *     manager / desktop moves the file(s) by default.
 *   - Holding Ctrl while dragging copies instead of moving.
 *   - After a successful move, the dragged items are immediately marked
 *     with red strikethrough in both views (same as File → Delete).
 *   - If app->general_enable_drag_drop is FALSE the function removes any
 *     previously installed drag source from both views (idempotent).
 *
 * Safe to call multiple times; repeated calls with the same enabled state
 * re-arm the source without creating duplicate signal handlers.
 */
void da_drag_drop_setup(AppState *app);

#endif /* DA_DRAG_DROP_H */
