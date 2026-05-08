#ifndef DISKATLAS_INI_H
#define DISKATLAS_INI_H

#include "app_state.h"

/** Path `diskatlas.ini` next to the running executable; caller must g_free. Returns NULL if unknown. */
gchar *da_ini_path(void);

/** Label of the last row in the file-view search combo; selecting it clears saved history. */
#define DA_SEARCH_HISTORY_CLEAR_ITEM "<clear>"

void da_ini_load_filetree(AppState *app);
void da_ini_save_filetree(const AppState *app);

/** Load `[search_history]` `queries` (colon-separated list in the file); caller must `g_strfreev`. Returns NULL if none. */
gchar **da_ini_search_history_load(gsize *n_out);

/** Persist search strings (newest first). Pass @a n=0 to remove the section. */
void da_ini_search_history_save(const gchar *const *items, gsize n);

#endif /* DISKATLAS_INI_H */
