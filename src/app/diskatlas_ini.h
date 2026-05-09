#ifndef DISKATLAS_INI_H
#define DISKATLAS_INI_H

#include "app_state.h"

/** Path `diskatlas.ini` next to the running executable; caller must g_free. Returns NULL if unknown. */
gchar *da_ini_path(void);

/** Label of the last row in the file-view search combo; selecting it clears saved history. */
#define DA_SEARCH_HISTORY_CLEAR_ITEM "<clear>"

typedef struct {
  gchar *name;
  gchar *color_hex;
  gchar *patterns_insensitive;
  gchar *patterns_sensitive;
} DaIniMimeCategory;

void da_ini_mime_category_destroy(gpointer cat);

/** Load `[mime_categories]` from `diskatlas.ini`; owns returned `GPtrArray` and elements (`DaIniMimeCategory`).
 * If the INI file cannot be read or has no `[mime_categories]` group, returns the built-in default categories
 * (see `da_default_mime_category_seeds.h`). */
GPtrArray *da_ini_mime_categories_load(void);

/** Replace `[mime_categories]` in `diskatlas.ini` (merges with other sections). `categories` may be empty. */
void da_ini_mime_categories_save(const GPtrArray *categories);

void da_ini_load_filetree(AppState *app);
void da_ini_save_filetree(const AppState *app);

/** Load `[interface]` `size_decimal_places` into `app->size_decimal_places` and apply formatting. Default 1. */
void da_ini_load_interface(AppState *app);
/** Persist `app->size_decimal_places` under `[interface]` (merge write). */
void da_ini_save_interface(const AppState *app);

/** Load `[search_history]` `queries` (colon-separated list in the file); caller must `g_strfreev`. Returns NULL if none. */
gchar **da_ini_search_history_load(gsize *n_out);

/** Persist search strings (newest first). Pass @a n=0 to remove the section. */
void da_ini_search_history_save(const gchar *const *items, gsize n);

#endif /* DISKATLAS_INI_H */
