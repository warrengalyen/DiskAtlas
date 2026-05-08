#ifndef DISKATLAS_INI_H
#define DISKATLAS_INI_H

#include "app_state.h"

/** Path `diskatlas.ini` next to the running executable; caller must g_free. Returns NULL if unknown. */
gchar *da_ini_path(void);

void da_ini_load_filetree(AppState *app);
void da_ini_save_filetree(const AppState *app);

#endif /* DISKATLAS_INI_H */
