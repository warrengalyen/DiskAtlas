#include "dm_mime_db.h"
#include "diskatlas_ini.h"
#include "dm_treemap_colors.h"

#include <string.h>
#include <stdlib.h>

/* -------------------------------------------------------------------------
 * Internal helpers
 * ---------------------------------------------------------------------- */

/** Parse a "#RRGGBB" or "#RRGGBBAA" hex string into a packed 0xRRGGBBAA uint32_t.
 *  Returns DISKATLAS_MIME_COLOR_FALLBACK for invalid/NULL input. */
static uint32_t parse_color_hex_to_rgba(const char *hex) {
  if (hex == NULL || hex[0] != '#') {
    return DISKATLAS_MIME_COLOR_FALLBACK;
  }
  size_t len = strlen(hex);
  if (len < 7) {
    return DISKATLAS_MIME_COLOR_FALLBACK;
  }
  char buf[3];
  buf[2] = '\0';

  buf[0] = hex[1]; buf[1] = hex[2];
  uint8_t r = (uint8_t)strtoul(buf, NULL, 16);
  buf[0] = hex[3]; buf[1] = hex[4];
  uint8_t g = (uint8_t)strtoul(buf, NULL, 16);
  buf[0] = hex[5]; buf[1] = hex[6];
  uint8_t b = (uint8_t)strtoul(buf, NULL, 16);
  uint8_t a = 0xFFu;
  if (len >= 9) {
    buf[0] = hex[7]; buf[1] = hex[8];
    a = (uint8_t)strtoul(buf, NULL, 16);
  }
  return ((uint32_t)r << 24) | ((uint32_t)g << 16) | ((uint32_t)b << 8) | (uint32_t)a;
}

/** Free a DmMimeCategory (GPtrArray element destructor). */
static void dm_mime_category_free(gpointer data) {
  if (data == NULL) return;
  DmMimeCategory *cat = (DmMimeCategory *)data;
  g_free(cat->name);
  g_free(cat);
}

/**
 * Insert key → value into the hash table only when key is not already present.
 * Takes ownership of key on success (inserts it); frees key on collision.
 * Returns TRUE if inserted.
 */
static gboolean ht_insert_if_absent(GHashTable *ht, gchar *key, gint value) {
  if (g_hash_table_contains(ht, key)) {
    g_free(key);
    return FALSE;
  }
  g_hash_table_insert(ht, key, GINT_TO_POINTER(value));
  return TRUE;
}

/**
 * Process one pattern line from a category at index cat_idx.
 * @param insensitive  TRUE → extension keys lowercased; bare names lowercased into filename_lookup.
 *                     FALSE → extension keys lowercased (extensions are inherently case-insensitive
 *                     in common use); bare names stored as-is (case-sensitive exact match).
 */
static void process_pattern_line(const char *line,
                                 gint cat_idx,
                                 gboolean insensitive,
                                 GHashTable *extension_lookup,
                                 GHashTable *filename_lookup) {
  if (line == NULL || *line == '\0') return;

  gchar *ext_key = NULL;

  if (g_str_has_prefix(line, "*.") && line[2] != '\0') {
    /* *.ext → .ext (lowercase for lookup) */
    ext_key = g_ascii_strdown(line + 1, -1);  /* skip '*', keep '.' */
    ht_insert_if_absent(extension_lookup, ext_key, cat_idx);
  } else if (line[0] == '.' && line[1] != '\0') {
    /* .ext → lowercase */
    ext_key = g_ascii_strdown(line, -1);
    ht_insert_if_absent(extension_lookup, ext_key, cat_idx);
  } else {
    /* Bare filename (e.g. "Makefile", "Dockerfile") */
    gchar *fn_key = insensitive ? g_ascii_strdown(line, -1) : g_strdup(line);
    ht_insert_if_absent(filename_lookup, fn_key, cat_idx);
  }
}

/* -------------------------------------------------------------------------
 * Public API
 * ---------------------------------------------------------------------- */

DmMimeDatabase *dm_mime_db_build(const GPtrArray *ini_categories) {
  DmMimeDatabase *db = g_new0(DmMimeDatabase, 1);
  db->categories = g_ptr_array_new_with_free_func(dm_mime_category_free);
  db->extension_lookup = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
  db->filename_lookup  = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);

  if (ini_categories == NULL) {
    return db;
  }

  for (guint i = 0; i < ini_categories->len; i++) {
    DaIniMimeCategory *ini_cat = (DaIniMimeCategory *)g_ptr_array_index(ini_categories, i);
    if (ini_cat == NULL) continue;

    DmMimeCategory *cat = g_new0(DmMimeCategory, 1);
    cat->name       = g_strdup(ini_cat->name != NULL ? ini_cat->name : "");
    cat->color_rgba = parse_color_hex_to_rgba(ini_cat->color_hex);
    g_ptr_array_add(db->categories, cat);

    gint cat_idx = (gint)(db->categories->len - 1u);

    /* Insensitive patterns */
    if (ini_cat->patterns_insensitive != NULL) {
      gchar **lines = g_strsplit(ini_cat->patterns_insensitive, "\n", -1);
      for (gchar **l = lines; *l != NULL; l++) {
        g_strstrip(*l);
        process_pattern_line(*l, cat_idx, TRUE, db->extension_lookup, db->filename_lookup);
      }
      g_strfreev(lines);
    }

    /* Sensitive patterns */
    if (ini_cat->patterns_sensitive != NULL) {
      gchar **lines = g_strsplit(ini_cat->patterns_sensitive, "\n", -1);
      for (gchar **l = lines; *l != NULL; l++) {
        g_strstrip(*l);
        process_pattern_line(*l, cat_idx, FALSE, db->extension_lookup, db->filename_lookup);
      }
      g_strfreev(lines);
    }
  }

  return db;
}

void dm_mime_db_free(DmMimeDatabase *db) {
  if (db == NULL) return;
  if (db->categories)       g_ptr_array_unref(db->categories);
  if (db->extension_lookup) g_hash_table_destroy(db->extension_lookup);
  if (db->filename_lookup)  g_hash_table_destroy(db->filename_lookup);
  g_free(db);
}

/* -------------------------------------------------------------------------
 * Classification helpers
 * ---------------------------------------------------------------------- */

/** Return pointer to the last '.' in @a name that starts an extension, or NULL.
 *  A leading dot (hidden file like ".bashrc") is not treated as an extension. */
static const char *dm_filename_get_extension(const char *name) {
  if (name == NULL) return NULL;
  const char *dot = strrchr(name, '.');
  if (dot == NULL || dot == name) return NULL;  /* no dot, or leading dot */
  return dot;
}

/** Return the basename (after the last '/' or '\') from a full path. */
static const char *dm_get_basename(const char *path) {
  if (path == NULL) return NULL;
  const char *p = strrchr(path, '/');
  if (p != NULL) path = p + 1;
#if defined(_WIN32) || defined(G_OS_WIN32)
  const char *q = strrchr(path, '\\');
  if (q != NULL) path = q + 1;
#endif
  return path;
}

/** Assign MIME color and treemap gradient caches for a non-directory node. */
static void dm_set_node_mime_rgba(file_node_t *fn, uint32_t rgba) {
  fn->mime_color_rgba = rgba;
  dm_file_node_compute_gradient_colors(fn, DM_TREEMAP_DEFAULT_GRADIENT_STRENGTH, DM_TREEMAP_DEFAULT_SHADOW_STRENGTH);
}

void dm_mime_db_classify_nodes(const DmMimeDatabase *db,
                               file_node_t *nodes, size_t count) {
  if (db == NULL || nodes == NULL || count == 0) return;

  for (size_t i = 0; i < count; i++) {
    file_node_t *fn = &nodes[i];
    uint32_t kind = fn->attributes & DISKATLAS_NODE_KIND_MASK;

    /* Directories keep mime_color_rgba=0; treemap renders them separately in gray. */
    if (kind == DISKATLAS_NODE_KIND_DIR) continue;

    const char *path = fn->path;
    if (path == NULL) {
      fn->mime_category_id = DISKATLAS_MIME_CATEGORY_UNKNOWN;
      dm_set_node_mime_rgba(fn, DISKATLAS_MIME_COLOR_FALLBACK);
      continue;
    }

    const char *base = dm_get_basename(path);
    if (base == NULL || *base == '\0') {
      fn->mime_category_id = DISKATLAS_MIME_CATEGORY_UNKNOWN;
      dm_set_node_mime_rgba(fn, DISKATLAS_MIME_COLOR_FALLBACK);
      continue;
    }

    /* 1. Exact case-sensitive filename lookup */
    gpointer val = g_hash_table_lookup(db->filename_lookup, base);
    if (val == NULL) {
      /* 1b. Lowercased basename lookup (for patterns_insensitive bare names) */
      gchar *base_lower = g_ascii_strdown(base, -1);
      val = g_hash_table_lookup(db->filename_lookup, base_lower);
      g_free(base_lower);
    }
    if (val != NULL) {
      gint idx = GPOINTER_TO_INT(val);
      DmMimeCategory *cat = (DmMimeCategory *)g_ptr_array_index(db->categories, (guint)idx);
      fn->mime_category_id = (uint8_t)(idx <= 0xFE ? idx : 0xFE);
      dm_set_node_mime_rgba(fn, cat->color_rgba);
      continue;
    }

    /* 2. Extension lookup (lowercase) */
    const char *ext = dm_filename_get_extension(base);
    if (ext == NULL) {
      fn->mime_category_id = DISKATLAS_MIME_CATEGORY_UNKNOWN;
      dm_set_node_mime_rgba(fn, DISKATLAS_MIME_COLOR_FALLBACK);
      continue;
    }

    gchar *ext_lower = g_ascii_strdown(ext, -1);
    val = g_hash_table_lookup(db->extension_lookup, ext_lower);
    g_free(ext_lower);

    if (val != NULL) {
      gint idx = GPOINTER_TO_INT(val);
      DmMimeCategory *cat = (DmMimeCategory *)g_ptr_array_index(db->categories, (guint)idx);
      fn->mime_category_id = (uint8_t)(idx <= 0xFE ? idx : 0xFE);
      dm_set_node_mime_rgba(fn, cat->color_rgba);
    } else {
      fn->mime_category_id = DISKATLAS_MIME_CATEGORY_UNKNOWN;
      dm_set_node_mime_rgba(fn, DISKATLAS_MIME_COLOR_FALLBACK);
    }
  }
}
