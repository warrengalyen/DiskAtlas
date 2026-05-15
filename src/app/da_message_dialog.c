#include "da_message_dialog.h"

void da_message_dialog_apply_layout(GtkWidget *dialog) {
  if (dialog == NULL || !GTK_IS_MESSAGE_DIALOG(dialog)) {
    return;
  }

  gtk_container_set_border_width(GTK_CONTAINER(dialog), DA_MESSAGE_DIALOG_BORDER);

  GtkWidget *msg_area = gtk_message_dialog_get_message_area(GTK_MESSAGE_DIALOG(dialog));
  if (msg_area != NULL && GTK_IS_BOX(msg_area)) {
    gtk_box_set_spacing(GTK_BOX(msg_area), DA_MESSAGE_DIALOG_SPACING);
  }

  GtkWidget *content = gtk_dialog_get_content_area(GTK_DIALOG(dialog));
  if (content != NULL && GTK_IS_BOX(content)) {
    gtk_box_set_spacing(GTK_BOX(content), DA_MESSAGE_DIALOG_SPACING);
  }
}
