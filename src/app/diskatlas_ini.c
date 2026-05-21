#if defined(__linux__)
#define _DEFAULT_SOURCE
#endif

#include <stddef.h>

#include <glib.h>
#include <gtk/gtk.h>

#include "diskatlas_ini.h"
#include "dm_treemap_colors.h"
#include "da_default_mime_categories.h"
#include "file_type_view.h"
#include "format_text.h"
#include "tree_view_model.h"

#if defined(G_OS_WIN32)
#ifndef DISKATLAS_INI_H
#define DISKATLAS_INI_H
#endif
#include <windows.h>
#include "volumes.h"
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
#define DA_KEY_VIEW_HIDDEN_FILES "view_hidden_files"
#define DA_SEC_GENERAL "general"
#define DA_KEY_ENABLE_RENAME "enable_rename"
#define DA_KEY_WIN32_EXPLORER_CTX_MENU "win32_explorer_context_menu"
#define DA_KEY_OPEN_FILE_DOUBLE_CLICK "open_file_double_click"
#define DA_KEY_FS_MONITOR "monitor_file_system"
#define DA_KEY_ENABLE_DRAG_DROP "enable_drag_and_drop"
#define DA_KEY_ALWAYS_RUN_AS_ADMIN "always_run_as_admin"
#define DA_KEY_HIDE_ADMIN_NTFS_NOTICE "hide_admin_ntfs_notice"

#define DA_SEC_EXPORT "export"
#define DA_KEY_EXPORT_TREEMAP_PNG_WIDTH "treemap_png_width"
#define DA_KEY_EXPORT_TREEMAP_PNG_HEIGHT "treemap_png_height"
#define DA_KEY_EXPORT_TREEMAP_PNG_GRAYSCALE "treemap_png_grayscale"
#define DA_KEY_EXPORT_TREEMAP_PNG_SHOW_FREE_SPACE "treemap_png_show_free_space"
#define DA_EXPORT_TREEMAP_PNG_DIM_MIN 100
#define DA_EXPORT_TREEMAP_PNG_DIM_MAX 16384

#define DA_KEY_FV_SORT_COL "file_view_tree_sort_column"
#define DA_KEY_FV_SORT_ORD "file_view_tree_sort_order"
#define DA_KEY_FV_WIDTHS "file_view_tree_column_widths"
#define DA_KEY_TV_SORT_COL "folder_tree_sort_column"
#define DA_KEY_TV_SORT_ORD "folder_tree_sort_order"
#define DA_KEY_TV_WIDTHS "folder_tree_column_widths"
#define DA_KEY_FT_SORT_COL "file_type_tree_sort_column"
#define DA_KEY_FT_SORT_ORD "file_type_tree_sort_order"
#define DA_KEY_FT_WIDTHS "file_type_tree_column_widths"

static gboolean da_ini_file_view_sort_col_ok(gint col) {
  return col == 0 || col == 1 || col == DA_COL_PCT || (col >= 3 && col <= 8);
}

static gboolean da_ini_folder_tree_sort_col_ok(gint col) {
  return col == DA_TV_COL_NAME || col == DA_TV_COL_PCT_VAL || col == DA_TV_COL_SIZE || col == DA_TV_COL_ALLOC ||
         col == DA_TV_COL_ITEMS || col == DA_TV_COL_FILES || col == DA_TV_COL_FOLDERS || col == DA_TV_COL_MODIFIED ||
         col == DA_TV_COL_ATTRS;
}

static gboolean da_ini_file_type_sort_col_ok(gint col) {
  return col == DA_FT_COL_EXT || col == DA_FT_COL_TYPE || col == DA_FT_COL_SIZE_RAW || col == DA_FT_COL_ALLOC_RAW ||
         col == DA_FT_COL_FILES_RAW;
}

static gboolean da_ini_sort_order_ok(gint ord) {
  return ord == (gint)GTK_SORT_ASCENDING || ord == (gint)GTK_SORT_DESCENDING;
}

static void da_ini_clear_interface_tree_prefs(AppState *app) {
  if (app == NULL) {
    return;
  }
  app->interface_ini_file_view_sort_set = FALSE;
  app->interface_ini_folder_tree_sort_set = FALSE;
  app->interface_ini_file_type_sort_set = FALSE;
  g_free(app->interface_ini_file_view_col_widths);
  app->interface_ini_file_view_col_widths = NULL;
  g_free(app->interface_ini_folder_tree_col_widths);
  app->interface_ini_folder_tree_col_widths = NULL;
  g_free(app->interface_ini_file_type_col_widths);
  app->interface_ini_file_type_col_widths = NULL;
}

static void da_ini_read_tree_prefs(GKeyFile *kf, AppState *app) {
  if (kf == NULL || app == NULL) {
    return;
  }
  GError *err = NULL;
  if (g_key_file_has_key(kf, DA_SEC_INTERFACE, DA_KEY_FV_SORT_COL, NULL)) {
    gint c = g_key_file_get_integer(kf, DA_SEC_INTERFACE, DA_KEY_FV_SORT_COL, &err);
    g_clear_error(&err);
    if (da_ini_file_view_sort_col_ok(c) || c == GTK_TREE_SORTABLE_UNSORTED_SORT_COLUMN_ID) {
      app->interface_ini_file_view_sort_col = c;
      app->interface_ini_file_view_sort_order = (gint)GTK_SORT_DESCENDING;
      if (g_key_file_has_key(kf, DA_SEC_INTERFACE, DA_KEY_FV_SORT_ORD, NULL)) {
        gint o = g_key_file_get_integer(kf, DA_SEC_INTERFACE, DA_KEY_FV_SORT_ORD, &err);
        g_clear_error(&err);
        if (da_ini_sort_order_ok(o)) {
          app->interface_ini_file_view_sort_order = o;
        }
      }
      app->interface_ini_file_view_sort_set = TRUE;
    }
  }
  if (g_key_file_has_key(kf, DA_SEC_INTERFACE, DA_KEY_FV_WIDTHS, NULL)) {
    gchar *w = g_key_file_get_string(kf, DA_SEC_INTERFACE, DA_KEY_FV_WIDTHS, &err);
    g_clear_error(&err);
    if (w != NULL && w[0] != '\0') {
      g_free(app->interface_ini_file_view_col_widths);
      app->interface_ini_file_view_col_widths = w;
    } else {
      g_free(w);
    }
  }

  if (g_key_file_has_key(kf, DA_SEC_INTERFACE, DA_KEY_TV_SORT_COL, NULL)) {
    gint c = g_key_file_get_integer(kf, DA_SEC_INTERFACE, DA_KEY_TV_SORT_COL, &err);
    g_clear_error(&err);
    if (da_ini_folder_tree_sort_col_ok(c) || c == GTK_TREE_SORTABLE_UNSORTED_SORT_COLUMN_ID) {
      app->interface_ini_folder_tree_sort_col = c;
      app->interface_ini_folder_tree_sort_order = (gint)GTK_SORT_ASCENDING;
      if (g_key_file_has_key(kf, DA_SEC_INTERFACE, DA_KEY_TV_SORT_ORD, NULL)) {
        gint o = g_key_file_get_integer(kf, DA_SEC_INTERFACE, DA_KEY_TV_SORT_ORD, &err);
        g_clear_error(&err);
        if (da_ini_sort_order_ok(o)) {
          app->interface_ini_folder_tree_sort_order = o;
        }
      }
      app->interface_ini_folder_tree_sort_set = TRUE;
    }
  }
  if (g_key_file_has_key(kf, DA_SEC_INTERFACE, DA_KEY_TV_WIDTHS, NULL)) {
    gchar *w = g_key_file_get_string(kf, DA_SEC_INTERFACE, DA_KEY_TV_WIDTHS, &err);
    g_clear_error(&err);
    if (w != NULL && w[0] != '\0') {
      g_free(app->interface_ini_folder_tree_col_widths);
      app->interface_ini_folder_tree_col_widths = w;
    } else {
      g_free(w);
    }
  }

  if (g_key_file_has_key(kf, DA_SEC_INTERFACE, DA_KEY_FT_SORT_COL, NULL)) {
    gint c = g_key_file_get_integer(kf, DA_SEC_INTERFACE, DA_KEY_FT_SORT_COL, &err);
    g_clear_error(&err);
    if (da_ini_file_type_sort_col_ok(c) || c == GTK_TREE_SORTABLE_UNSORTED_SORT_COLUMN_ID) {
      app->interface_ini_file_type_sort_col = c;
      app->interface_ini_file_type_sort_order = (gint)GTK_SORT_DESCENDING;
      if (g_key_file_has_key(kf, DA_SEC_INTERFACE, DA_KEY_FT_SORT_ORD, NULL)) {
        gint o = g_key_file_get_integer(kf, DA_SEC_INTERFACE, DA_KEY_FT_SORT_ORD, &err);
        g_clear_error(&err);
        if (da_ini_sort_order_ok(o)) {
          app->interface_ini_file_type_sort_order = o;
        }
      }
      app->interface_ini_file_type_sort_set = TRUE;
    }
  }
  if (g_key_file_has_key(kf, DA_SEC_INTERFACE, DA_KEY_FT_WIDTHS, NULL)) {
    gchar *w = g_key_file_get_string(kf, DA_SEC_INTERFACE, DA_KEY_FT_WIDTHS, &err);
    g_clear_error(&err);
    if (w != NULL && w[0] != '\0') {
      g_free(app->interface_ini_file_type_col_widths);
      app->interface_ini_file_type_col_widths = w;
    } else {
      g_free(w);
    }
  }
}

static gchar *da_ini_join_column_widths(GtkTreeView *tv) {
  if (tv == NULL) {
    return NULL;
  }
  gint n = gtk_tree_view_get_n_columns(tv);
  if (n <= 0) {
    return NULL;
  }
  GString *s = g_string_new(NULL);
  for (gint i = 0; i < n; i++) {
    GtkTreeViewColumn *c = gtk_tree_view_get_column(tv, i);
    gint w = 0;
    if (c != NULL) {
      w = gtk_tree_view_column_get_fixed_width(c);
      if (w <= 0) {
        w = gtk_tree_view_column_get_width(c);
      }
    }
    if (i > 0) {
      g_string_append_c(s, ',');
    }
    g_string_append_printf(s, "%d", w);
  }
  return g_string_free(s, FALSE);
}

static void da_ini_apply_width_csv(GtkTreeView *tv, const gchar *csv) {
  if (tv == NULL || csv == NULL || csv[0] == '\0') {
    return;
  }
  gchar **parts = g_strsplit(csv, ",", -1);
  if (parts == NULL) {
    return;
  }
  gint n = gtk_tree_view_get_n_columns(tv);
  for (gint i = 0; parts[i] != NULL && i < n; i++) {
    char *end = NULL;
    glong v = g_ascii_strtoll(parts[i], &end, 10);
    if (end == parts[i]) {
      continue;
    }
    GtkTreeViewColumn *col = gtk_tree_view_get_column(tv, i);
    if (col == NULL) {
      break;
    }
    gint minw = gtk_tree_view_column_get_min_width(col);
    gint wi = (gint)v;
    if (wi < minw) {
      wi = minw;
    }
    if (wi > 16000) {
      wi = 16000;
    }
    gtk_tree_view_column_set_fixed_width(col, wi);
  }
  g_strfreev(parts);
}

void da_ini_apply_interface_tree_columns(AppState *app) {
  if (app == NULL) {
    return;
  }
  if (app->tree != NULL && GTK_IS_TREE_VIEW(app->tree)) {
    GtkTreeView *tv = GTK_TREE_VIEW(app->tree);
    if (app->interface_ini_file_view_col_widths != NULL) {
      da_ini_apply_width_csv(tv, app->interface_ini_file_view_col_widths);
    }
    if (app->interface_ini_file_view_sort_set && app->flat_list_model != NULL) {
      gint c = app->interface_ini_file_view_sort_col;
      gint o = app->interface_ini_file_view_sort_order;
      if (c == GTK_TREE_SORTABLE_UNSORTED_SORT_COLUMN_ID) {
        gtk_tree_sortable_set_sort_column_id(GTK_TREE_SORTABLE(app->flat_list_model),
                                             GTK_TREE_SORTABLE_UNSORTED_SORT_COLUMN_ID, GTK_SORT_ASCENDING);
      } else if (da_ini_file_view_sort_col_ok(c) && da_ini_sort_order_ok(o)) {
        gtk_tree_sortable_set_sort_column_id(GTK_TREE_SORTABLE(app->flat_list_model), c, (GtkSortType)o);
      }
    } else if (app->flat_list_model != NULL) {
      gtk_tree_sortable_set_sort_column_id(GTK_TREE_SORTABLE(app->flat_list_model), DA_COL_ALLOCATED,
                                           GTK_SORT_DESCENDING);
    }
    g_free(app->interface_ini_file_view_col_widths);
    app->interface_ini_file_view_col_widths = NULL;
    app->interface_ini_file_view_sort_set = FALSE;
  }

  if (app->tree_view != NULL && GTK_IS_TREE_VIEW(app->tree_view) && app->tree_view_store != NULL) {
    GtkTreeView *tv = GTK_TREE_VIEW(app->tree_view);
    if (app->interface_ini_folder_tree_col_widths != NULL) {
      da_ini_apply_width_csv(tv, app->interface_ini_folder_tree_col_widths);
    }
    if (app->interface_ini_folder_tree_sort_set) {
      gint c = app->interface_ini_folder_tree_sort_col;
      gint o = app->interface_ini_folder_tree_sort_order;
      if (c == GTK_TREE_SORTABLE_UNSORTED_SORT_COLUMN_ID) {
        gtk_tree_sortable_set_sort_column_id(GTK_TREE_SORTABLE(app->tree_view_store),
                                             GTK_TREE_SORTABLE_UNSORTED_SORT_COLUMN_ID, GTK_SORT_ASCENDING);
      } else if (da_ini_folder_tree_sort_col_ok(c) && da_ini_sort_order_ok(o)) {
        gtk_tree_sortable_set_sort_column_id(GTK_TREE_SORTABLE(app->tree_view_store), c, (GtkSortType)o);
      }
    } else {
      gtk_tree_sortable_set_sort_column_id(GTK_TREE_SORTABLE(app->tree_view_store), DA_TV_COL_ALLOC,
                                           GTK_SORT_DESCENDING);
    }
    g_free(app->interface_ini_folder_tree_col_widths);
    app->interface_ini_folder_tree_col_widths = NULL;
    app->interface_ini_folder_tree_sort_set = FALSE;
  }

  if (app->file_type_tree != NULL && GTK_IS_TREE_VIEW(app->file_type_tree)) {
    GtkTreeView *tv = GTK_TREE_VIEW(app->file_type_tree);
    GtkTreeModel *md = gtk_tree_view_get_model(tv);
    if (app->interface_ini_file_type_col_widths != NULL) {
      da_ini_apply_width_csv(tv, app->interface_ini_file_type_col_widths);
    }
    if (app->interface_ini_file_type_sort_set && md != NULL && GTK_IS_TREE_SORTABLE(md)) {
      gint c = app->interface_ini_file_type_sort_col;
      gint o = app->interface_ini_file_type_sort_order;
      if (c == GTK_TREE_SORTABLE_UNSORTED_SORT_COLUMN_ID) {
        gtk_tree_sortable_set_sort_column_id(GTK_TREE_SORTABLE(md),
                                             GTK_TREE_SORTABLE_UNSORTED_SORT_COLUMN_ID, GTK_SORT_ASCENDING);
      } else if (da_ini_file_type_sort_col_ok(c) && da_ini_sort_order_ok(o)) {
        gtk_tree_sortable_set_sort_column_id(GTK_TREE_SORTABLE(md), c, (GtkSortType)o);
      }
    } else if (md != NULL && GTK_IS_TREE_SORTABLE(md)) {
      gtk_tree_sortable_set_sort_column_id(GTK_TREE_SORTABLE(md), DA_FT_COL_ALLOC_RAW, GTK_SORT_DESCENDING);
    }
    g_free(app->interface_ini_file_type_col_widths);
    app->interface_ini_file_type_col_widths = NULL;
    app->interface_ini_file_type_sort_set = FALSE;
  }
}

static void da_ini_save_tree_view_prefs(GKeyFile *kf, const AppState *app) {
  if (kf == NULL || app == NULL) {
    return;
  }
  if (app->tree != NULL && GTK_IS_TREE_VIEW(app->tree) && app->flat_list_model != NULL) {
    GtkTreeSortable *st = GTK_TREE_SORTABLE(app->flat_list_model);
    gint sc = 0;
    GtkSortType ord = GTK_SORT_ASCENDING;
    if (gtk_tree_sortable_get_sort_column_id(st, &sc, &ord)) {
      g_key_file_set_integer(kf, DA_SEC_INTERFACE, DA_KEY_FV_SORT_COL, sc);
      g_key_file_set_integer(kf, DA_SEC_INTERFACE, DA_KEY_FV_SORT_ORD, (gint)ord);
    } else {
      g_key_file_set_integer(kf, DA_SEC_INTERFACE, DA_KEY_FV_SORT_COL, GTK_TREE_SORTABLE_UNSORTED_SORT_COLUMN_ID);
      g_key_file_set_integer(kf, DA_SEC_INTERFACE, DA_KEY_FV_SORT_ORD, (gint)GTK_SORT_ASCENDING);
    }
    gchar *w = da_ini_join_column_widths(GTK_TREE_VIEW(app->tree));
    if (w != NULL) {
      g_key_file_set_string(kf, DA_SEC_INTERFACE, DA_KEY_FV_WIDTHS, w);
      g_free(w);
    }
  }

  if (app->tree_view != NULL && GTK_IS_TREE_VIEW(app->tree_view) && app->tree_view_store != NULL) {
    GtkTreeSortable *st = GTK_TREE_SORTABLE(app->tree_view_store);
    gint sc = 0;
    GtkSortType ord = GTK_SORT_ASCENDING;
    if (gtk_tree_sortable_get_sort_column_id(st, &sc, &ord)) {
      g_key_file_set_integer(kf, DA_SEC_INTERFACE, DA_KEY_TV_SORT_COL, sc);
      g_key_file_set_integer(kf, DA_SEC_INTERFACE, DA_KEY_TV_SORT_ORD, (gint)ord);
    } else {
      g_key_file_set_integer(kf, DA_SEC_INTERFACE, DA_KEY_TV_SORT_COL, GTK_TREE_SORTABLE_UNSORTED_SORT_COLUMN_ID);
      g_key_file_set_integer(kf, DA_SEC_INTERFACE, DA_KEY_TV_SORT_ORD, (gint)GTK_SORT_ASCENDING);
    }
    gchar *w = da_ini_join_column_widths(GTK_TREE_VIEW(app->tree_view));
    if (w != NULL) {
      g_key_file_set_string(kf, DA_SEC_INTERFACE, DA_KEY_TV_WIDTHS, w);
      g_free(w);
    }
  }

  if (app->file_type_tree != NULL && GTK_IS_TREE_VIEW(app->file_type_tree)) {
    GtkTreeModel *md = gtk_tree_view_get_model(GTK_TREE_VIEW(app->file_type_tree));
    if (md != NULL && GTK_IS_TREE_SORTABLE(md)) {
      GtkTreeSortable *st = GTK_TREE_SORTABLE(md);
      gint sc = 0;
      GtkSortType ord = GTK_SORT_ASCENDING;
      if (gtk_tree_sortable_get_sort_column_id(st, &sc, &ord)) {
        g_key_file_set_integer(kf, DA_SEC_INTERFACE, DA_KEY_FT_SORT_COL, sc);
        g_key_file_set_integer(kf, DA_SEC_INTERFACE, DA_KEY_FT_SORT_ORD, (gint)ord);
      } else {
        g_key_file_set_integer(kf, DA_SEC_INTERFACE, DA_KEY_FT_SORT_COL, GTK_TREE_SORTABLE_UNSORTED_SORT_COLUMN_ID);
        g_key_file_set_integer(kf, DA_SEC_INTERFACE, DA_KEY_FT_SORT_ORD, (gint)GTK_SORT_ASCENDING);
      }
    }
    gchar *w = da_ini_join_column_widths(GTK_TREE_VIEW(app->file_type_tree));
    if (w != NULL) {
      g_key_file_set_string(kf, DA_SEC_INTERFACE, DA_KEY_FT_WIDTHS, w);
      g_free(w);
    }
  }
}

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
  da_ini_clear_interface_tree_prefs(app);
  app->treemap_style = DM_TREEMAP_STYLE_INIT_DEFAULT;
  app->interface_alternate_row_colors = FALSE;
  app->interface_show_header = TRUE;
  app->interface_show_file_types = TRUE;
  app->interface_show_treemap = TRUE;
  app->interface_treemap_show_free_space = FALSE;
  app->interface_treemap_show_labels = TRUE;
  app->interface_view_hidden_files = TRUE;
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
      if (g_key_file_has_key(kf, DA_SEC_INTERFACE, DA_KEY_VIEW_HIDDEN_FILES, NULL)) {
        gboolean vh = g_key_file_get_boolean(kf, DA_SEC_INTERFACE, DA_KEY_VIEW_HIDDEN_FILES, &err);
        g_clear_error(&err);
        app->interface_view_hidden_files = vh;
      }
      if (g_key_file_has_key(kf, DA_SEC_INTERFACE, DA_KEY_SIZE_DISPLAY_FORMAT, NULL)) {
        gint sf = g_key_file_get_integer(kf, DA_SEC_INTERFACE, DA_KEY_SIZE_DISPLAY_FORMAT, &err);
        g_clear_error(&err);
        if (sf >= (gint)DA_SIZE_DISPLAY_DYNAMIC && sf <= (gint)DA_SIZE_DISPLAY_TB) {
          app->interface_size_display_format = sf;
        }
      }
      da_ini_read_tree_prefs(kf, app);
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
  /* Default: double-click opens files with the default application. */
  app->general_open_file_double_click = TRUE;
  /* Default: file system monitor enabled. */
  app->general_fs_monitor = TRUE;
  /* Default: drag-and-drop enabled. */
  app->general_enable_drag_drop = TRUE;
  /* Default: do not force elevation on startup. */
  app->general_always_run_as_admin = FALSE;
  /* Default: show NTFS/admin notice banner when not elevated. */
  app->general_hide_admin_ntfs_notice = FALSE;
  gchar *path = da_ini_path();
  if (path == NULL) {
    return;
  }
  GKeyFile *kf = g_key_file_new();
  gboolean hide_notice_in_diskatlas_ini = FALSE;
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
    if (g_key_file_has_key(kf, DA_SEC_GENERAL, DA_KEY_OPEN_FILE_DOUBLE_CLICK, NULL)) {
      gboolean od = g_key_file_get_boolean(kf, DA_SEC_GENERAL, DA_KEY_OPEN_FILE_DOUBLE_CLICK, &err);
      g_clear_error(&err);
      app->general_open_file_double_click = od;
    }
    if (g_key_file_has_key(kf, DA_SEC_GENERAL, DA_KEY_FS_MONITOR, NULL)) {
      gboolean fm = g_key_file_get_boolean(kf, DA_SEC_GENERAL, DA_KEY_FS_MONITOR, &err);
      g_clear_error(&err);
      app->general_fs_monitor = fm;
    }
    if (g_key_file_has_key(kf, DA_SEC_GENERAL, DA_KEY_ENABLE_DRAG_DROP, NULL)) {
      gboolean dd = g_key_file_get_boolean(kf, DA_SEC_GENERAL, DA_KEY_ENABLE_DRAG_DROP, &err);
      g_clear_error(&err);
      app->general_enable_drag_drop = dd;
    }
    if (g_key_file_has_key(kf, DA_SEC_GENERAL, DA_KEY_ALWAYS_RUN_AS_ADMIN, NULL)) {
      gboolean ar = g_key_file_get_boolean(kf, DA_SEC_GENERAL, DA_KEY_ALWAYS_RUN_AS_ADMIN, &err);
      g_clear_error(&err);
      app->general_always_run_as_admin = ar;
    }
    if (g_key_file_has_key(kf, DA_SEC_GENERAL, DA_KEY_HIDE_ADMIN_NTFS_NOTICE, NULL)) {
      gboolean hn = g_key_file_get_boolean(kf, DA_SEC_GENERAL, DA_KEY_HIDE_ADMIN_NTFS_NOTICE, &err);
      g_clear_error(&err);
      app->general_hide_admin_ntfs_notice = hn;
      hide_notice_in_diskatlas_ini = TRUE;
    }
  }
#if defined(G_OS_WIN32)
  if (!hide_notice_in_diskatlas_ini && da_win32_admin_ntfs_notice_saved_hidden()) {
    app->general_hide_admin_ntfs_notice = TRUE;
  }
#endif
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
  g_key_file_set_boolean(kf, DA_SEC_GENERAL, DA_KEY_OPEN_FILE_DOUBLE_CLICK,
                         app->general_open_file_double_click);
  g_key_file_set_boolean(kf, DA_SEC_GENERAL, DA_KEY_FS_MONITOR, app->general_fs_monitor);
  g_key_file_set_boolean(kf, DA_SEC_GENERAL, DA_KEY_ENABLE_DRAG_DROP, app->general_enable_drag_drop);
  g_key_file_set_boolean(kf, DA_SEC_GENERAL, DA_KEY_ALWAYS_RUN_AS_ADMIN, app->general_always_run_as_admin);
  g_key_file_set_boolean(kf, DA_SEC_GENERAL, DA_KEY_HIDE_ADMIN_NTFS_NOTICE, app->general_hide_admin_ntfs_notice);

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
  g_key_file_set_boolean(kf, DA_SEC_INTERFACE, DA_KEY_VIEW_HIDDEN_FILES, app->interface_view_hidden_files);
  {
    gint sf = app->interface_size_display_format;
    if (sf < (gint)DA_SIZE_DISPLAY_DYNAMIC || sf > (gint)DA_SIZE_DISPLAY_TB) {
      sf = DA_SIZE_DISPLAY_DYNAMIC;
    }
    g_key_file_set_integer(kf, DA_SEC_INTERFACE, DA_KEY_SIZE_DISPLAY_FORMAT, sf);
  }

  da_ini_save_tree_view_prefs(kf, app);

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

static gint da_ini_clamp_treemap_export_dim(gint v) {
  if (v < DA_EXPORT_TREEMAP_PNG_DIM_MIN) {
    return DA_EXPORT_TREEMAP_PNG_DIM_MIN;
  }
  if (v > DA_EXPORT_TREEMAP_PNG_DIM_MAX) {
    return DA_EXPORT_TREEMAP_PNG_DIM_MAX;
  }
  return v;
}

void da_ini_export_treemap_png_load(gint *width, gint *height, gboolean *grayscale, gboolean *show_free_space,
                                    gboolean show_free_space_fallback) {
  if (width == NULL || height == NULL || grayscale == NULL || show_free_space == NULL) {
    return;
  }
  gchar *path = da_ini_path();
  if (path == NULL) {
    *show_free_space = show_free_space_fallback;
    return;
  }
  GKeyFile *kf = g_key_file_new();
  if (!da_key_file_load_merged(kf, path) || !g_key_file_has_group(kf, DA_SEC_EXPORT)) {
    g_key_file_unref(kf);
    g_free(path);
    *show_free_space = show_free_space_fallback;
    return;
  }
  g_free(path);

  GError *err = NULL;
  if (g_key_file_has_key(kf, DA_SEC_EXPORT, DA_KEY_EXPORT_TREEMAP_PNG_WIDTH, NULL)) {
    gint w = g_key_file_get_integer(kf, DA_SEC_EXPORT, DA_KEY_EXPORT_TREEMAP_PNG_WIDTH, &err);
    g_clear_error(&err);
    *width = da_ini_clamp_treemap_export_dim(w);
  }
  if (g_key_file_has_key(kf, DA_SEC_EXPORT, DA_KEY_EXPORT_TREEMAP_PNG_HEIGHT, NULL)) {
    gint h = g_key_file_get_integer(kf, DA_SEC_EXPORT, DA_KEY_EXPORT_TREEMAP_PNG_HEIGHT, &err);
    g_clear_error(&err);
    *height = da_ini_clamp_treemap_export_dim(h);
  }
  if (g_key_file_has_key(kf, DA_SEC_EXPORT, DA_KEY_EXPORT_TREEMAP_PNG_GRAYSCALE, NULL)) {
    gboolean g = g_key_file_get_boolean(kf, DA_SEC_EXPORT, DA_KEY_EXPORT_TREEMAP_PNG_GRAYSCALE, &err);
    g_clear_error(&err);
    *grayscale = g;
  }
  if (g_key_file_has_key(kf, DA_SEC_EXPORT, DA_KEY_EXPORT_TREEMAP_PNG_SHOW_FREE_SPACE, NULL)) {
    gboolean f = g_key_file_get_boolean(kf, DA_SEC_EXPORT, DA_KEY_EXPORT_TREEMAP_PNG_SHOW_FREE_SPACE, &err);
    g_clear_error(&err);
    *show_free_space = f;
  } else {
    *show_free_space = show_free_space_fallback;
  }
  g_key_file_unref(kf);
}

void da_ini_export_treemap_png_save(gint width, gint height, gboolean grayscale, gboolean show_free_space) {
  gchar *path = da_ini_path();
  if (path == NULL) {
    return;
  }
  gint w = da_ini_clamp_treemap_export_dim(width);
  gint h = da_ini_clamp_treemap_export_dim(height);
  GKeyFile *kf = g_key_file_new();
  (void)da_key_file_load_merged(kf, path);
  g_key_file_set_integer(kf, DA_SEC_EXPORT, DA_KEY_EXPORT_TREEMAP_PNG_WIDTH, w);
  g_key_file_set_integer(kf, DA_SEC_EXPORT, DA_KEY_EXPORT_TREEMAP_PNG_HEIGHT, h);
  g_key_file_set_boolean(kf, DA_SEC_EXPORT, DA_KEY_EXPORT_TREEMAP_PNG_GRAYSCALE, grayscale);
  g_key_file_set_boolean(kf, DA_SEC_EXPORT, DA_KEY_EXPORT_TREEMAP_PNG_SHOW_FREE_SPACE, show_free_space);

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
