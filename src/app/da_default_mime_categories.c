#include "diskatlas_ini.h"
#include "da_default_mime_categories.h"

#define DA_DEFAULT_MIME_CATEGORY_SEEDS_DEFINE
#include "da_default_mime_category_seeds.h"
#undef DA_DEFAULT_MIME_CATEGORY_SEEDS_DEFINE

void da_default_mime_categories_append_seeds(GPtrArray *cats) {
  if (cats == NULL) {
    return;
  }
  for (gsize i = 0; i < G_N_ELEMENTS(da_default_mime_category_seeds_table); i++) {
    const DaDefaultMimeCategorySeed *s = &da_default_mime_category_seeds_table[i];
    DaIniMimeCategory *c = g_new0(DaIniMimeCategory, 1);
    c->name = g_strdup(s->name);
    c->color_hex = g_strdup(s->color_hex);
    c->patterns_insensitive = g_strdup(s->patterns_insensitive != NULL ? s->patterns_insensitive : "");
    c->patterns_sensitive = g_strdup(s->patterns_sensitive != NULL ? s->patterns_sensitive : "");
    g_ptr_array_add(cats, c);
  }
}

void da_default_mime_categories_replace_all(GPtrArray *cats) {
  if (cats == NULL) {
    return;
  }
  g_ptr_array_set_size(cats, 0);
  da_default_mime_categories_append_seeds(cats);
}
