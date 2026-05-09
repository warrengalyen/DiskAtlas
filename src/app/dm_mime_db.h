#ifndef DM_MIME_DB_H
#define DM_MIME_DB_H

#include <stddef.h>
#include <glib.h>
#include "../include/diskatlas.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * One runtime MIME category: a display name and a packed RGBA color.
 */
typedef struct DmMimeCategory {
  char    *name;        /**< Owned; display name (e.g. "Audio"). */
  uint32_t color_rgba;  /**< Packed 0xRRGGBBAA, ready for treemap rendering. */
} DmMimeCategory;

/**
 * Runtime MIME classification database built from DaIniMimeCategory settings.
 * All lookups are O(1) GHashTable queries.
 */
typedef struct DmMimeDatabase {
  GPtrArray  *categories;       /**< DmMimeCategory *; index 0..N-1. */
  GHashTable *extension_lookup; /**< gchar* lowercase ".ext" → GINT_TO_POINTER(cat_idx). */
  GHashTable *filename_lookup;  /**< gchar* exact/lower basename → GINT_TO_POINTER(cat_idx). */
} DmMimeDatabase;

/**
 * Build a DmMimeDatabase from an array of DaIniMimeCategory (as returned by
 * da_ini_mime_categories_load).  First-category-wins precedence for duplicate keys.
 * Pass NULL to get an empty but valid database.
 */
DmMimeDatabase *dm_mime_db_build(const GPtrArray *ini_categories);

/** Free all resources owned by @a db (including the struct itself). Safe to call with NULL. */
void            dm_mime_db_free(DmMimeDatabase *db);

/**
 * Classify every non-directory node in @a nodes, writing mime_category_id and
 * mime_color_rgba.  Directories are skipped (left at 0; treemap renders them in gray).
 * O(1) per node via hash table lookups.
 */
void            dm_mime_db_classify_nodes(const DmMimeDatabase *db,
                                          file_node_t *nodes, size_t count);

#ifdef __cplusplus
}
#endif

#endif /* DM_MIME_DB_H */
