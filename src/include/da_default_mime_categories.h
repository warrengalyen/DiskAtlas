#ifndef DA_DEFAULT_MIME_CATEGORIES_H
#define DA_DEFAULT_MIME_CATEGORIES_H

#include <glib.h>

/** One built-in MIME category row (see `da_default_mime_category_seeds.h` for editable defaults). */
typedef struct {
  const gchar *name;
  const gchar *color_hex;
  /** One pattern per line; case-insensitive matching (typical `*.ext` / `.ext`). */
  const gchar *patterns_insensitive;
  /** One pattern per line; case-sensitive (Linux); may include extension-less names like `Makefile`. */
  const gchar *patterns_sensitive;
} DaDefaultMimeCategorySeed;

/** Append built-in categories (deep copy as `DaIniMimeCategory`) to @a categories. */
void da_default_mime_categories_append_seeds(GPtrArray *categories);

/** Clear @a categories then append built-in rows (for a future “reset to defaults” action). */
void da_default_mime_categories_replace_all(GPtrArray *categories);

#endif  /* DA_DEFAULT_MIME_CATEGORIES_H */
