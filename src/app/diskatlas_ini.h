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

/** Load `[interface]` (size decimals, size display format, treemap gradients, alternate row colors, etc.). */
void da_ini_load_interface(AppState *app);
/** Persist `[interface]` keys (merge write). */
void da_ini_save_interface(const AppState *app);

/** Apply tree column widths and sort state loaded by `da_ini_load_interface` (call after all tree views have columns). */
void da_ini_apply_interface_tree_columns(AppState *app);

/** Load `[general]` (enable_rename, open_file_double_click, hide_admin_ntfs_notice, etc.). */
void da_ini_load_general(AppState *app);
/** Persist `[general]` keys (merge write). */
void da_ini_save_general(const AppState *app);

/** Load `[search_history]` `queries` (colon-separated list in the file); caller must `g_strfreev`. Returns NULL if none. */
gchar **da_ini_search_history_load(gsize *n_out);

/** Persist search strings (newest first). Pass @a n=0 to remove the section. */
void da_ini_search_history_save(const gchar *const *items, gsize n);

/**
 * Read `[export]` treemap PNG keys into @a width, @a height, @a grayscale, @a show_free_space.
 * Each value is only updated when its key exists and is valid; dimensions are clamped to [100, 16384].
 * If `treemap_png_show_free_space` is absent, @a show_free_space is set to @a show_free_space_fallback.
 */
void da_ini_export_treemap_png_load(gint *width, gint *height, gboolean *grayscale, gboolean *show_free_space,
                                    gboolean show_free_space_fallback);

/** Merge-write `[export]` treemap PNG keys (dimensions clamped to [100, 16384]). */
void da_ini_export_treemap_png_save(gint width, gint height, gboolean grayscale, gboolean show_free_space);

#endif /* DISKATLAS_INI_H */
