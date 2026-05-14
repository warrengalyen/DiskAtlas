#ifndef DA_COLOR_PICKER_DIALOG_H
#define DA_COLOR_PICKER_DIALOG_H

#include <glib.h>
#include <gtk/gtk.h>

/**
 * Modal color picker (Photoshop-style plane + strip, HSB/RGB + hex).
 * If @original_for_compare is NULL, "current" matches the initial color.
 * On TRUE, @out receives the chosen color (alpha forced to 1).
 */
gboolean da_color_picker_dialog_run(GtkWindow *parent, const GdkRGBA *initial,
                                      const GdkRGBA *original_for_compare, GdkRGBA *out);

#endif /* DA_COLOR_PICKER_DIALOG_H */
