#ifndef SETTINGS_MIME_TAB_H
#define SETTINGS_MIME_TAB_H

#include <glib.h>
#include <gtk/gtk.h>

typedef struct DaSettingsMimeCtx DaSettingsMimeCtx;

DaSettingsMimeCtx *da_settings_mime_tab_bind(GtkBuilder *builder);
void da_settings_mime_tab_free(DaSettingsMimeCtx *ctx);
gboolean da_settings_mime_tab_save(DaSettingsMimeCtx *ctx, GtkWindow *parent);

#endif /* SETTINGS_MIME_TAB_H */
