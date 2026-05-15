#ifndef DA_MESSAGE_DIALOG_H
#define DA_MESSAGE_DIALOG_H

#include <gtk/gtk.h>

/** Padding and spacing applied to all GtkMessageDialog instances in the app. */
#define DA_MESSAGE_DIALOG_BORDER   15
#define DA_MESSAGE_DIALOG_SPACING  10

/** Set border width and box spacing on the message and content areas. */
void da_message_dialog_apply_layout(GtkWidget *dialog);

#endif
