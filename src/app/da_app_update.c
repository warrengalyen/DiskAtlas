#include "da_app_update.h"

#include <gtk/gtk.h>

#include "app_state.h"
#include "da_message_dialog.h"
#include "da_update_config.h"
#include "gtk_libupdate.h"

void da_help_menu_check_for_updates(GtkMenuItem *item, gpointer user_data) {
  (void)item;
  AppState *app = (AppState *)user_data;
  if (app == NULL || app->window == NULL) {
    return;
  }

  GtkLibupdateConfig cfg = {
      .manifest_url = DA_UPDATE_MANIFEST_URL,
      .libupdate_app_name = "diskatlas",
      .display_name = "DiskAtlas",
      .temp_dir_template = "diskatlas_up_XXXXXX",
      .configure_message_dialog = da_message_dialog_apply_layout,
      .disabled_build_explanation =
          "Your administrator can enable it by setting DISKATLAS_UPDATE_MANIFEST_URL "
          "when configuring the application.",
  };

  gtk_libupdate_check_for_updates(GTK_WINDOW(app->window), &cfg);
}
