#include <stddef.h>

#include <glib.h>
#include <gtk/gtk.h>

#include "diskatlas_ini.h"
#include "dm_treemap_colors.h"
#include "da_default_mime_categories.h"
#include "format_text.h"

#if defined(G_OS_WIN32)
#ifndef DISKATLAS_INI_H
#define DISKATLAS_INI_H
#endif
#include <windows.h>
#elif defined(__linux__)
#include <unistd.h>
#elif defined(__APPLE__)
#include <mach-o/dyld.h>
#endif

#define DA_INI_NAME "diskatlas.ini"
#define DA_SEC_FILETREE "filetree"
#define DA_SEC_SEARCH_HISTORY "search_history"
#define DA_KEY_SEARCH_QUERIES "queries"
#define DA_SEC_MIME_CATEGORIES "mime_categories"
#define DA_SEC_INTERFACE "interface"
#define DA_KEY_SIZE_DECIMAL_PLACES "size_decimal_places"
#define DA_KEY_SIZE_DISPLAY_FORMAT "size_display_format"
#define DA_KEY_TREEMAP_TILE_GRADIENTS "treemap_tile_gradients"
#define DA_KEY_ALTERNATE_ROW_COLORS "alternate_row_colors"
#define DA_KEY_SHOW_HEADER "show_header"
#define DA_KEY_SHOW_FILE_TYPES "show_file_types"
#define DA_KEY_SHOW_TREEMAP "show_treemap"
#define DA_KEY_TREEMAP_SHOW_FREE_SPACE "treemap_show_free_space"
#define DA_KEY_TREEMAP_SHOW_LABELS "treemap_show_labels"
#define DA_SEC_GENERAL "general"
#define DA_KEY_ENABLE_RENAME "enable_rename"
#define DA_KEY_WIN32_EXPLORER_CTX_MENU "win32_explorer_context_menu"

static gchar *da_exe_dir_utf8(void) {
#if defined(G_OS_WIN32)
  WCHAR wbuf[32768];
  DWORD n = GetModuleFileNameW(NULL, wbuf, (DWORD)(G_N_ELEMENTS(wbuf) - 1));
  if (n == 0 || n >= G_N_ELEMENTS(wbuf) - 1) {
    return NULL;
  }
  wbuf[n] = L'\0';
  gchar *exe = g_utf16_to_utf8((const gunichar2 *)wbuf, -1, NULL, NULL, NULL);
  if (exe == NULL) {
    return NULL;
  }
  gchar *dir = g_path_get_dirname(exe);
  g_free(exe);
  return dir;
#elif defined(__linux__)
  char buf[4096];
  ssize_t len = readlink("/proc/self/exe", buf, sizeof(buf) - 1u);
  if (len <= 0) {
    return NULL;
  }
  buf[(size_t)len] = '\0';
  return g_path_get_dirname(buf);
#elif defined(__APPLE__)
  char stack[4096];
  uint32_t size = (uint32_t)sizeof(stack);
  if (_NSGetExecutablePath(stack, &size) == 0) {
    return g_path_get_dirname(stack);
  }
  gchar *dyn = g_malloc((gsize)size);
  if (dyn == NULL) {
    return NULL;
  }
  if (_NSGetExecutablePath(dyn, &size) != 0) {
    g_free(dyn);
    return NULL;
  }
  gchar *dir = g_path_get_dirname(dyn);
  g_free(dyn);
  return dir;
#else
  return NULL;
#endif  /* DISKATLAS_INI_H */
}

gchar *da_ini_path(void) {
  gchar *dir = da_exe_dir_utf8();
  if (dir == NULL) {
    return NULL;
  }
  gchar *path = g_build_filename(dir, DA_INI_NAME, NULL);
  g_free(dir);
  return path;
}

static gboolean da_key_file_load_merged(GKeyFile *kf, const gchar *path) {
  if (kf == NULL || path == NULL) {
    return FALSE;
  }
  /* Colon separates list values (e.g. `search_history` `queries`); semicolon may appear in filter text. */
  g_key_file_set_list_separator(kf, ':');
  return g_key_file_load_from_file(kf, path, G_KEY_FILE_KEEP_COMMENTS | G_KEY_FILE_KEEP_TRANSLATIONS, NULL);
}

void da_ini_mime_category_destroy(gpointer cat) {
  DaIniMimeCategory *c = (DaIniMimeCategory *)cat;
  if (c == NULL) {
    return;
  }
  g_free(c->name);
  g_free(c->color_hex);
  g_free(c->patterns_insensitive);
  g_free(c->patterns_sensitive);
  g_free(c);
}

GPtrArray *da_ini_mime_categories_load(void) {
  GPtrArray *arr = g_ptr_array_new_with_free_func(da_ini_mime_category_destroy);
  gchar *path = da_ini_path();
  if (path == NULL) {
    da_default_mime_categories_append_seeds(arr);
    return arr;
  }
  GKeyFile *kf = g_key_file_new();
  if (!da_key_file_load_merged(kf, path)) {
    g_key_file_unref(kf);
    g_free(path);
    da_default_mime_categories_append_seeds(arr);
    return arr;
  }
  g_free(path);

  if (!g_key_file_has_group(kf, DA_SEC_MIME_CATEGORIES)) {
    g_key_file_unref(kf);
    da_default_mime_categories_append_seeds(arr);
    return arr;
  }

  GError *err = NULL;
  gint count = g_key_file_get_integer(kf, DA_SEC_MIME_CATEGORIES, "count", &err);
  if (err != NULL) {
    g_clear_error(&err);
    count = 0;
  }
  if (count < 0) {
    count = 0;
  }

  for (gint i = 0; i < count; i++) {
    gchar *nk = g_strdup_printf("name%d", i);
    gchar *ck = g_strdup_printf("color%d", i);
    gchar *ik = g_strdup_printf("patterns_insensitive%d", i);
    gchar *sk = g_strdup_printf("patterns_sensitive%d", i);

    DaIniMimeCategory *c = g_new0(DaIniMimeCategory, 1);
    c->name = g_key_file_get_string(kf, DA_SEC_MIME_CATEGORIES, nk, &err);
    if (err != NULL) {
      g_clear_error(&err);
      c->name = g_strdup("");
    }
    c->color_hex = g_key_file_get_string(kf, DA_SEC_MIME_CATEGORIES, ck, &err);
    if (err != NULL) {
      g_clear_error(&err);
      c->color_hex = g_strdup("#808080");
    }
    c->patterns_insensitive = g_key_file_get_string(kf, DA_SEC_MIME_CATEGORIES, ik, &err);
    if (err != NULL) {
      g_clear_error(&err);
      c->patterns_insensitive = g_strdup("");
    }
    c->patterns_sensitive = g_key_file_get_string(kf, DA_SEC_MIME_CATEGORIES, sk, &err);
    if (err != NULL) {
      g_clear_error(&err);
      c->patterns_sensitive = g_strdup("");
    }

    g_ptr_array_add(arr, c);
    g_free(nk);
    g_free(ck);
    g_free(ik);
    g_free(sk);
  }

  g_key_file_unref(kf);
  return arr;
}

void da_ini_mime_categories_save(const GPtrArray *categories) {
  gchar *path = da_ini_path();
  if (path == NULL) {
    return;
  }
  GKeyFile *kf = g_key_file_new();
  (void)da_key_file_load_merged(kf, path);

  GError *rm_err = NULL;
  g_key_file_remove_group(kf, DA_SEC_MIME_CATEGORIES, &rm_err);
  g_clear_error(&rm_err);

  gsize n = (categories != NULL) ? categories->len : 0;
  g_key_file_set_integer(kf, DA_SEC_MIME_CATEGORIES, "count", (gint)n);

  for (gsize i = 0; i < n; i++) {
    DaIniMimeCategory *c = g_ptr_array_index(categories, i);
    gchar *nk = g_strdup_printf("name%zu", i);
    gchar *ck = g_strdup_printf("color%zu", i);
    gchar *ik = g_strdup_printf("patterns_insensitive%zu", i);
    gchar *sk = g_strdup_printf("patterns_sensitive%zu", i);
    g_key_file_set_string(kf, DA_SEC_MIME_CATEGORIES, nk, c->name != NULL ? c->name : "");
    g_key_file_set_string(kf, DA_SEC_MIME_CATEGORIES, ck, c->color_hex != NULL ? c->color_hex : "");
    g_key_file_set_string(kf, DA_SEC_MIME_CATEGORIES, ik,
                          c->patterns_insensitive != NULL ? c->patterns_insensitive : "");
    g_key_file_set_string(kf, DA_SEC_MIME_CATEGORIES, sk,
                          c->patterns_sensitive != NULL ? c->patterns_sensitive : "");
    g_free(nk);
    g_free(ck);
    g_free(ik);
    g_free(sk);
  }

  gsize len = 0;
  gchar *data = g_key_file_to_data(kf, &len, NULL);
  g_key_file_unref(kf);
  if (data == NULL) {
    g_free(path);
    return;
  }

  gchar *dir = g_path_get_dirname(path);
  if (dir != NULL) {
    g_mkdir_with_parents(dir, 0755);
    g_free(dir);
  }

  GError *werr = NULL;
  g_file_set_contents(path, data, (gssize)len, &werr);
  g_clear_error(&werr);
  g_free(data);
  g_free(path);
}

void da_ini_load_interface(AppState *app) {
  if (app == NULL) {
    return;
  }
  app->treemap_style = DM_TREEMAP_STYLE_INIT_DEFAULT;
  app->interface_alternate_row_colors = FALSE;
  app->interface_show_header = TRUE;
  app->interface_show_file_types = TRUE;
  app->interface_show_treemap = TRUE;
  app->interface_treemap_show_free_space = FALSE;
  app->interface_treemap_show_labels = TRUE;
  app->interface_size_display_format = DA_SIZE_DISPLAY_DYNAMIC;

  gint places = 1;
  gchar *path = da_ini_path();
  if (path != NULL) {
    GKeyFile *kf = g_key_file_new();
    if (da_key_file_load_merged(kf, path) && g_key_file_has_group(kf, DA_SEC_INTERFACE)) {
      GError *err = NULL;
      if (g_key_file_has_key(kf, DA_SEC_INTERFACE, DA_KEY_SIZE_DECIMAL_PLACES, NULL)) {
        gint v = g_key_file_get_integer(kf, DA_SEC_INTERFACE, DA_KEY_SIZE_DECIMAL_PLACES, &err);
        g_clear_error(&err);
        if (v >= 0 && v <= 4) {
          places = v;
        }
      }
      if (g_key_file_has_key(kf, DA_SEC_INTERFACE, DA_KEY_TREEMAP_TILE_GRADIENTS, NULL)) {
        gboolean tg = g_key_file_get_boolean(kf, DA_SEC_INTERFACE, DA_KEY_TREEMAP_TILE_GRADIENTS, &err);
        g_clear_error(&err);
        app->treemap_style.enable_tile_gradients = tg;
      }
      if (g_key_file_has_key(kf, DA_SEC_INTERFACE, DA_KEY_ALTERNATE_ROW_COLORS, NULL)) {
        gboolean arc = g_key_file_get_boolean(kf, DA_SEC_INTERFACE, DA_KEY_ALTERNATE_ROW_COLORS, &err);
        g_clear_error(&err);
        app->interface_alternate_row_colors = arc;
      }
      if (g_key_file_has_key(kf, DA_SEC_INTERFACE, DA_KEY_SHOW_HEADER, NULL)) {
        gboolean sh = g_key_file_get_boolean(kf, DA_SEC_INTERFACE, DA_KEY_SHOW_HEADER, &err);
        g_clear_error(&err);
        app->interface_show_header = sh;
      }
      if (g_key_file_has_key(kf, DA_SEC_INTERFACE, DA_KEY_SHOW_FILE_TYPES, NULL)) {
        gboolean ft = g_key_file_get_boolean(kf, DA_SEC_INTERFACE, DA_KEY_SHOW_FILE_TYPES, &err);
        g_clear_error(&err);
        app->interface_show_file_types = ft;
      }
      if (g_key_file_has_key(kf, DA_SEC_INTERFACE, DA_KEY_SHOW_TREEMAP, NULL)) {
        gboolean tm = g_key_file_get_boolean(kf, DA_SEC_INTERFACE, DA_KEY_SHOW_TREEMAP, &err);
        g_clear_error(&err);
        app->interface_show_treemap = tm;
      }
      if (g_key_file_has_key(kf, DA_SEC_INTERFACE, DA_KEY_TREEMAP_SHOW_FREE_SPACE, NULL)) {
        gboolean fs = g_key_file_get_boolean(kf, DA_SEC_INTERFACE, DA_KEY_TREEMAP_SHOW_FREE_SPACE, &err);
        g_clear_error(&err);
        app->interface_treemap_show_free_space = fs;
      }
      if (g_key_file_has_key(kf, DA_SEC_INTERFACE, DA_KEY_TREEMAP_SHOW_LABELS, NULL)) {
        gboolean sl = g_key_file_get_boolean(kf, DA_SEC_INTERFACE, DA_KEY_TREEMAP_SHOW_LABELS, &err);
        g_clear_error(&err);
        app->interface_treemap_show_labels = sl;
      }
      if (g_key_file_has_key(kf, DA_SEC_INTERFACE, DA_KEY_SIZE_DISPLAY_FORMAT, NULL)) {
        gint sf = g_key_file_get_integer(kf, DA_SEC_INTERFACE, DA_KEY_SIZE_DISPLAY_FORMAT, &err);
        g_clear_error(&err);
        if (sf >= (gint)DA_SIZE_DISPLAY_DYNAMIC && sf <= (gint)DA_SIZE_DISPLAY_TB) {
          app->interface_size_display_format = sf;
        }
      }
    }
    g_key_file_unref(kf);
    g_free(path);
  }
  app->size_decimal_places = places;
  da_format_bytes_set_decimal_places(places);
  da_format_bytes_set_display_format(app->interface_size_display_format);
}

void da_ini_load_general(AppState *app) {
  if (app == NULL) {
    return;
  }
  /* Default: renaming disabled so F2 cannot trigger accidental renames. */
  app->general_enable_rename = FALSE;
  /* Default: Windows Explorer context menu integration enabled. */
  app->general_win32_explorer_context_menu = TRUE;
  gchar *path = da_ini_path();
  if (path == NULL) {
    return;
  }
  GKeyFile *kf = g_key_file_new();
  if (da_key_file_load_merged(kf, path) && g_key_file_has_group(kf, DA_SEC_GENERAL)) {
    GError *err = NULL;
    if (g_key_file_has_key(kf, DA_SEC_GENERAL, DA_KEY_ENABLE_RENAME, NULL)) {
      gboolean er = g_key_file_get_boolean(kf, DA_SEC_GENERAL, DA_KEY_ENABLE_RENAME, &err);
      g_clear_error(&err);
      app->general_enable_rename = er;
    }
    if (g_key_file_has_key(kf, DA_SEC_GENERAL, DA_KEY_WIN32_EXPLORER_CTX_MENU, NULL)) {
      gboolean v = g_key_file_get_boolean(kf, DA_SEC_GENERAL, DA_KEY_WIN32_EXPLORER_CTX_MENU, &err);
      g_clear_error(&err);
      app->general_win32_explorer_context_menu = v;
    }
  }
  g_key_file_unref(kf);
  g_free(path);
}

void da_ini_save_general(const AppState *app) {
  if (app == NULL) {
    return;
  }
  gchar *path = da_ini_path();
  if (path == NULL) {
    return;
  }
  GKeyFile *kf = g_key_file_new();
  (void)da_key_file_load_merged(kf, path);
  g_key_file_set_boolean(kf, DA_SEC_GENERAL, DA_KEY_ENABLE_RENAME, app->general_enable_rename);
  g_key_file_set_boolean(kf, DA_SEC_GENERAL, DA_KEY_WIN32_EXPLORER_CTX_MENU,
                         app->general_win32_explorer_context_menu);

  gsize len = 0;
  gchar *data = g_key_file_to_data(kf, &len, NULL);
  g_key_file_unref(kf);
  if (data == NULL) {
    g_free(path);
    return;
  }

  gchar *dir = g_path_get_dirname(path);
  if (dir != NULL) {
    g_mkdir_with_parents(dir, 0755);
    g_free(dir);
  }

  GError *werr = NULL;
  g_file_set_contents(path, data, (gssize)len, &werr);
  g_clear_error(&werr);
  g_free(data);
  g_free(path);
}

void da_ini_save_interface(const AppState *app) {
  if (app == NULL) {
    return;
  }
  gchar *path = da_ini_path();
  if (path == NULL) {
    return;
  }
  gint places = app->size_decimal_places;
  if (places < 0) {
    places = 0;
  } else if (places > 4) {
    places = 4;
  }

  GKeyFile *kf = g_key_file_new();
  (void)da_key_file_load_merged(kf, path);
  g_key_file_set_integer(kf, DA_SEC_INTERFACE, DA_KEY_SIZE_DECIMAL_PLACES, places);
  g_key_file_set_boolean(kf, DA_SEC_INTERFACE, DA_KEY_TREEMAP_TILE_GRADIENTS,
                         app->treemap_style.enable_tile_gradients);
  g_key_file_set_boolean(kf, DA_SEC_INTERFACE, DA_KEY_ALTERNATE_ROW_COLORS, app->interface_alternate_row_colors);
  g_key_file_set_boolean(kf, DA_SEC_INTERFACE, DA_KEY_SHOW_HEADER, app->interface_show_header);
  g_key_file_set_boolean(kf, DA_SEC_INTERFACE, DA_KEY_SHOW_FILE_TYPES, app->interface_show_file_types);
  g_key_file_set_boolean(kf, DA_SEC_INTERFACE, DA_KEY_SHOW_TREEMAP, app->interface_show_treemap);
  g_key_file_set_boolean(kf, DA_SEC_INTERFACE, DA_KEY_TREEMAP_SHOW_FREE_SPACE, app->interface_treemap_show_free_space);
  g_key_file_set_boolean(kf, DA_SEC_INTERFACE, DA_KEY_TREEMAP_SHOW_LABELS, app->interface_treemap_show_labels);
  {
    gint sf = app->interface_size_display_format;
    if (sf < (gint)DA_SIZE_DISPLAY_DYNAMIC || sf > (gint)DA_SIZE_DISPLAY_TB) {
      sf = DA_SIZE_DISPLAY_DYNAMIC;
    }
    g_key_file_set_integer(kf, DA_SEC_INTERFACE, DA_KEY_SIZE_DISPLAY_FORMAT, sf);
  }

  gsize len = 0;
  gchar *data = g_key_file_to_data(kf, &len, NULL);
  g_key_file_unref(kf);
  if (data == NULL) {
    g_free(path);
    return;
  }

  gchar *dir = g_path_get_dirname(path);
  if (dir != NULL) {
    g_mkdir_with_parents(dir, 0755);
    g_free(dir);
  }

  GError *werr = NULL;
  g_file_set_contents(path, data, (gssize)len, &werr);
  g_clear_error(&werr);
  g_free(data);
  g_free(path);
}

void da_ini_load_filetree(AppState *app) {
  if (app == NULL) {
    return;
  }
  gchar *path = da_ini_path();
  if (path == NULL) {
    return;
  }
  GKeyFile *kf = g_key_file_new();
  if (!da_key_file_load_merged(kf, path)) {
    g_key_file_unref(kf);
    g_free(path);
    return;
  }
  g_free(path);

  if (!g_key_file_has_group(kf, DA_SEC_FILETREE)) {
    g_key_file_unref(kf);
    return;
  }

  GError *err = NULL;

  if (g_key_file_has_key(kf, DA_SEC_FILETREE, "match_entire_path", NULL)) {
    gboolean entire = g_key_file_get_boolean(kf, DA_SEC_FILETREE, "match_entire_path", &err);
    g_clear_error(&err);
    if (app->match_entire_path_radio != NULL && GTK_IS_TOGGLE_BUTTON(app->match_entire_path_radio)) {
      gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(app->match_entire_path_radio), entire);
    } else if (app->match_filename_only_radio != NULL && GTK_IS_TOGGLE_BUTTON(app->match_filename_only_radio)) {
      gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(app->match_filename_only_radio), !entire);
    }
  }

  if (g_key_file_has_key(kf, DA_SEC_FILETREE, "display_max_index", NULL)) {
    gint ix = g_key_file_get_integer(kf, DA_SEC_FILETREE, "display_max_index", &err);
    g_clear_error(&err);
    if (app->combo_display_max != NULL && GTK_IS_COMBO_BOX(app->combo_display_max)) {
      if (ix >= 0 && ix <= 4) {
        gtk_combo_box_set_active(GTK_COMBO_BOX(app->combo_display_max), ix);
      }
    }
  }

  if (g_key_file_has_key(kf, DA_SEC_FILETREE, "show_folders", NULL)) {
    gboolean v = g_key_file_get_boolean(kf, DA_SEC_FILETREE, "show_folders", &err);
    g_clear_error(&err);
    if (app->show_folders_check != NULL && GTK_IS_TOGGLE_BUTTON(app->show_folders_check)) {
      gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(app->show_folders_check), v);
    }
  }

  if (g_key_file_has_key(kf, DA_SEC_FILETREE, "duplicates_mode", NULL)) {
    gint mode = g_key_file_get_integer(kf, DA_SEC_FILETREE, "duplicates_mode", &err);
    g_clear_error(&err);
    if (app->duplicates_file_combo != NULL && GTK_IS_COMBO_BOX(app->duplicates_file_combo)) {
      if (mode >= 0 && mode <= 2) {
        gtk_combo_box_set_active(GTK_COMBO_BOX(app->duplicates_file_combo), mode);
      }
    }
  }

  if (g_key_file_has_key(kf, DA_SEC_FILETREE, "duplicates_only", NULL)) {
    gboolean v = g_key_file_get_boolean(kf, DA_SEC_FILETREE, "duplicates_only", &err);
    g_clear_error(&err);
    if (app->duplicates_only_check != NULL && GTK_IS_TOGGLE_BUTTON(app->duplicates_only_check)) {
      gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(app->duplicates_only_check), v);
    }
  }

  g_key_file_unref(kf);
}

void da_ini_save_filetree(const AppState *app) {
  if (app == NULL) {
    return;
  }
  gchar *path = da_ini_path();
  if (path == NULL) {
    return;
  }

  GKeyFile *kf = g_key_file_new();
  (void)da_key_file_load_merged(kf, path);

  gboolean entire = FALSE;
  if (app->match_entire_path_radio != NULL && GTK_IS_TOGGLE_BUTTON(app->match_entire_path_radio)) {
    entire = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(app->match_entire_path_radio));
  }
  g_key_file_set_boolean(kf, DA_SEC_FILETREE, "match_entire_path", entire);

  gint display_ix = 3;
  if (app->combo_display_max != NULL && GTK_IS_COMBO_BOX(app->combo_display_max)) {
    gint ix = gtk_combo_box_get_active(GTK_COMBO_BOX(app->combo_display_max));
    if (ix >= 0 && ix <= 4) {
      display_ix = ix;
    }
  }
  g_key_file_set_integer(kf, DA_SEC_FILETREE, "display_max_index", display_ix);

  gboolean show_folders = FALSE;
  if (app->show_folders_check != NULL && GTK_IS_TOGGLE_BUTTON(app->show_folders_check)) {
    show_folders = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(app->show_folders_check));
  }
  g_key_file_set_boolean(kf, DA_SEC_FILETREE, "show_folders", show_folders);

  gint dup_mode = 2;
  if (app->duplicates_file_combo != NULL && GTK_IS_COMBO_BOX(app->duplicates_file_combo)) {
    gint m = gtk_combo_box_get_active(GTK_COMBO_BOX(app->duplicates_file_combo));
    if (m >= 0 && m <= 2) {
      dup_mode = m;
    }
  }
  g_key_file_set_integer(kf, DA_SEC_FILETREE, "duplicates_mode", dup_mode);

  gboolean dup_only = FALSE;
  if (app->duplicates_only_check != NULL && GTK_IS_TOGGLE_BUTTON(app->duplicates_only_check)) {
    dup_only = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(app->duplicates_only_check));
  }
  g_key_file_set_boolean(kf, DA_SEC_FILETREE, "duplicates_only", dup_only);

  gsize len = 0;
  gchar *data = g_key_file_to_data(kf, &len, NULL);
  g_key_file_unref(kf);
  if (data == NULL) {
    g_free(path);
    return;
  }

  gchar *dir = g_path_get_dirname(path);
  if (dir != NULL) {
    g_mkdir_with_parents(dir, 0755);
    g_free(dir);
  }

  GError *werr = NULL;
  g_file_set_contents(path, data, (gssize)len, &werr);
  g_clear_error(&werr);
  g_free(data);
  g_free(path);
}

gchar **da_ini_search_history_load(gsize *n_out) {
  if (n_out != NULL) {
    *n_out = 0;
  }
  gchar *path = da_ini_path();
  if (path == NULL) {
    return NULL;
  }
  GKeyFile *kf = g_key_file_new();
  if (!da_key_file_load_merged(kf, path)) {
    g_key_file_unref(kf);
    g_free(path);
    return NULL;
  }
  g_free(path);
  if (!g_key_file_has_group(kf, DA_SEC_SEARCH_HISTORY) ||
      !g_key_file_has_key(kf, DA_SEC_SEARCH_HISTORY, DA_KEY_SEARCH_QUERIES, NULL)) {
    g_key_file_unref(kf);
    return NULL;
  }
  GError *err = NULL;
  gsize n = 0;
  gchar **list = g_key_file_get_string_list(kf, DA_SEC_SEARCH_HISTORY, DA_KEY_SEARCH_QUERIES, &n, &err);
  g_key_file_unref(kf);
  if (err != NULL) {
    g_clear_error(&err);
    g_strfreev(list);
    return NULL;
  }
  if (n_out != NULL) {
    *n_out = n;
  }
  return list;
}

void da_ini_search_history_save(const gchar *const *items, gsize n) {
  gchar *path = da_ini_path();
  if (path == NULL) {
    return;
  }
  GKeyFile *kf = g_key_file_new();
  (void)da_key_file_load_merged(kf, path);
  if (n == 0) {
    g_key_file_remove_group(kf, DA_SEC_SEARCH_HISTORY, NULL);
  } else {
    if (items == NULL) {
      g_key_file_unref(kf);
      g_free(path);
      return;
    }
    g_key_file_set_string_list(kf, DA_SEC_SEARCH_HISTORY, DA_KEY_SEARCH_QUERIES, items, n);
  }
  gsize len = 0;
  gchar *data = g_key_file_to_data(kf, &len, NULL);
  g_key_file_unref(kf);
  if (data == NULL) {
    g_free(path);
    return;
  }
  gchar *dir = g_path_get_dirname(path);
  if (dir != NULL) {
    g_mkdir_with_parents(dir, 0755);
    g_free(dir);
  }
  GError *werr = NULL;
  g_file_set_contents(path, data, (gssize)len, &werr);
  g_clear_error(&werr);
  g_free(data);
  g_free(path);
}
