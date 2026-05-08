#include <ctype.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include <glib.h>
#include <gtk/gtk.h>

#include "diskatlas.h"
#include "file_tree_model.h"
#include "format_text.h"

static const char *utf8_basename_ptr(const char *path) {
  const char *base = path ? path : "";
  for (const char *q = base; *q; q++) {
    if (*q == '/' || *q == '\\') {
      base = q + 1;
    }
  }
  return base;
}

static gboolean filter_has_wildcard(const char *filter) {
  if (!filter) {
    return FALSE;
  }
  for (const char *s = filter; *s; s = g_utf8_next_char(s)) {
    gunichar c = g_utf8_get_char(s);
    if (c == '*' || c == '?') {
      return TRUE;
    }
  }
  return FALSE;
}

static gboolean utf8_contains_ci_folded(const char *hay_fold, const char *needle_fold) {
  if (!needle_fold || !needle_fold[0]) {
    return TRUE;
  }
  if (!hay_fold) {
    hay_fold = "";
  }
  return strstr(hay_fold, needle_fold) != NULL;
}

/* Glob on UTF-8 case-folded strings: '*' = any substring, '?' = one Unicode character. */
static gboolean wild_match_folded_utf8(const char *pat, const char *str, int depth) {
  if (depth++ > 10000) {
    return FALSE;
  }
  if (*pat == '\0') {
    return *str == '\0';
  }

  gunichar pc = g_utf8_get_char(pat);
  if (pc == '*') {
    const char *next_pat = g_utf8_next_char(pat);
    if (*next_pat == '\0') {
      return TRUE;
    }
    for (;;) {
      if (wild_match_folded_utf8(next_pat, str, depth)) {
        return TRUE;
      }
      if (*str == '\0') {
        return FALSE;
      }
      str = g_utf8_next_char(str);
    }
  }
  if (pc == '?') {
    if (*str == '\0') {
      return FALSE;
    }
    return wild_match_folded_utf8(g_utf8_next_char(pat), g_utf8_next_char(str), depth);
  }
  if (*str == '\0') {
    return FALSE;
  }
  gunichar sc = g_utf8_get_char(str);
  if (pc != sc) {
    return FALSE;
  }
  return wild_match_folded_utf8(g_utf8_next_char(pat), g_utf8_next_char(str), depth);
}

gboolean da_duplicates_only(const AppState *app) {
  if (app == NULL || app->duplicates_only_check == NULL) {
    return FALSE;
  }
  return gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(app->duplicates_only_check));
}

gboolean da_view_uses_filtered_pool(const AppState *app) {
  if (app == NULL) {
    return FALSE;
  }
  return app->filter_active || da_duplicates_only(app);
}

gboolean da_show_folders_in_file_list(const AppState *app) {
  if (app == NULL || app->show_folders_check == NULL) {
    return FALSE;
  }
  return gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(app->show_folders_check));
}

gboolean da_node_shown_in_file_view(const AppState *app, const scan_results_view_t *v, size_t nid) {
  if (v == NULL || v->nodes == NULL || nid >= v->count) {
    return FALSE;
  }
  uint32_t kind = v->nodes[nid].attributes & DISKATLAS_NODE_KIND_MASK;
  if (kind == DISKATLAS_NODE_KIND_DIR && !da_show_folders_in_file_list(app)) {
    return FALSE;
  }
  return TRUE;
}

/* ASCII fast-path: returns TRUE if every byte of s is a pure ASCII character (< 0x80). */
static gboolean str_is_ascii(const char *s) {
  if (!s) {
    return TRUE;
  }
  for (const unsigned char *p = (const unsigned char *)s; *p; p++) {
    if (*p >= 0x80u) {
      return FALSE;
    }
  }
  return TRUE;
}

/* Case-insensitive substring search restricted to ASCII; no heap alloc. */
static gboolean ascii_contains_ci(const char *hay, const char *needle) {
  if (!needle || !needle[0]) {
    return TRUE;
  }
  if (!hay) {
    return FALSE;
  }
  size_t nl = strlen(needle);
  for (; *hay; hay++) {
    size_t i;
    for (i = 0; i < nl; i++) {
      if (tolower((unsigned char)hay[i]) != tolower((unsigned char)needle[i])) {
        break;
      }
    }
    if (i == nl) {
      return TRUE;
    }
  }
  return FALSE;
}

static gboolean utf8_folded_matches_filter(const char *hay_utf8, const char *filter_utf8) {
  if (!filter_utf8 || !filter_utf8[0]) {
    return TRUE;
  }
  const char *hay = hay_utf8 ? hay_utf8 : "";

  /* ASCII fast-path: avoid two heap allocations for the common all-ASCII case. */
  if (!filter_has_wildcard(filter_utf8) && str_is_ascii(hay) && str_is_ascii(filter_utf8)) {
    return ascii_contains_ci(hay, filter_utf8);
  }

  gchar *hay_fold = g_utf8_casefold(hay, -1);
  gchar *fi_fold = g_utf8_casefold(filter_utf8, -1);
  gboolean ok;
  if (filter_has_wildcard(filter_utf8)) {
    ok = wild_match_folded_utf8(fi_fold, hay_fold, 0);
  } else {
    ok = utf8_contains_ci_folded(hay_fold, fi_fold);
  }
  g_free(hay_fold);
  g_free(fi_fold);
  return ok;
}

gboolean da_utf8_basename_matches_filter(const char *path_utf8, const char *filter_utf8) {
  return utf8_folded_matches_filter(utf8_basename_ptr(path_utf8), filter_utf8);
}

gboolean da_utf8_file_view_filter_matches(const AppState *app, const char *path_utf8) {
  if (app == NULL) {
    return FALSE;
  }
  const char *path = path_utf8 != NULL ? path_utf8 : "";
  gboolean entire = (app->match_entire_path_radio != NULL &&
                     gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(app->match_entire_path_radio)));
  if (entire) {
    return utf8_folded_matches_filter(path, app->filter_text);
  }
  return utf8_folded_matches_filter(utf8_basename_ptr(path), app->filter_text);
}

size_t da_source_pool_count(const AppState *app) {
  if (app == NULL) {
    return 0;
  }
  return da_view_uses_filtered_pool(app) ? app->filtered_count : app->master_count;
}

size_t da_source_count(const AppState *app) {
  if (app == NULL) {
    return 0;
  }
  size_t pool = da_source_pool_count(app);
  size_t cap = app->display_max_entries;
  if (cap == 0) {
    return pool;
  }
  return pool < cap ? pool : cap;
}

size_t da_source_at(const AppState *app, size_t i) {
  if (da_view_uses_filtered_pool(app)) {
    return app->filtered_indices[i];
  }
  return app->master_indices[i];
}

gboolean da_tree_lp_to_scan_nid(AppState *app, gint64 lp, size_t *out_nid) {
  if (out_nid == NULL) {
    return FALSE;
  }
  if (lp == DA_TREE_LP_PLACEHOLDER) {
    *out_nid = SIZE_MAX;
    return TRUE;
  }
  if (lp > 0) {
    *out_nid = (size_t)(lp - 1);
    return TRUE;
  }
  /* Negative LP (dup-group parent row) is no longer used with FlatListModel,
   * but handle it defensively for any residual code paths. */
  if (lp < 0 && app != NULL && app->scan != NULL) {
    uint32_t gid = (uint32_t)(-lp);
    size_t nmem = 0;
    const size_t *mp = diskatlas_dup_group_members(app->scan, gid, &nmem);
    scan_results_view_t v = scan_get_results(app->scan);
    if (mp != NULL && nmem > 0 && mp[0] < v.count) {
      *out_nid = mp[0];
      return TRUE;
    }
  }
  return FALSE;
}
