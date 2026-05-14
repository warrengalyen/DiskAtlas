#include <string.h>

#include <glib.h>
#include <gtk/gtk.h>

#include "da_color_picker_dialog.h"
#include "diskatlas_ini.h"
#include "settings_mime_tab.h"

struct DaSettingsMimeCtx {
  GtkWindow *settings_window;
  GtkListBox *category_list;
  GtkEntry *name_entry;
  GtkWidget *color_btn;
  GtkEntry *hex_entry;
  GtkTextView *pat_ins;
  GtkTextView *pat_sens;
  GtkWidget *sensitive_label;
  GtkWidget *sensitive_scrolled;
  GPtrArray *categories;
  gint selected_idx;
  gboolean frozen;
};

static void da_mime_set_details_sensitive(DaSettingsMimeCtx *ctx, gboolean sensitive) {
  if (ctx == NULL) {
    return;
  }
  if (ctx->name_entry != NULL) {
    gtk_widget_set_sensitive(GTK_WIDGET(ctx->name_entry), sensitive);
  }
  if (ctx->color_btn != NULL) {
    gtk_widget_set_sensitive(GTK_WIDGET(ctx->color_btn), sensitive);
  }
  if (ctx->hex_entry != NULL) {
    gtk_widget_set_sensitive(GTK_WIDGET(ctx->hex_entry), sensitive);
  }
  if (ctx->pat_ins != NULL) {
    gtk_widget_set_sensitive(GTK_WIDGET(ctx->pat_ins), sensitive);
  }
  if (ctx->pat_sens != NULL) {
    gtk_widget_set_sensitive(GTK_WIDGET(ctx->pat_sens), sensitive);
  }
}

static gchar *da_text_view_get_text(GtkTextView *tv) {
  GtkTextBuffer *buf = gtk_text_view_get_buffer(tv);
  GtkTextIter a, b;
  gtk_text_buffer_get_start_iter(buf, &a);
  gtk_text_buffer_get_end_iter(buf, &b);
  return gtk_text_buffer_get_text(buf, &a, &b, FALSE);
}

static void da_text_view_set_text(GtkTextView *tv, const gchar *text) {
  GtkTextBuffer *buf = gtk_text_view_get_buffer(tv);
  gtk_text_buffer_set_text(buf, text != NULL ? text : "", -1);
}

static gchar *da_hex_from_rgba(const GdkRGBA *rgba) {
  return g_strdup_printf("#%02X%02X%02X", (guint)(rgba->red * 255.0 + 0.5), (guint)(rgba->green * 255.0 + 0.5),
                         (guint)(rgba->blue * 255.0 + 0.5));
}

static gboolean da_mime_color_swatch_draw(GtkWidget *w, cairo_t *cr, gpointer user_data) {
  (void)user_data;
  GdkRGBA rgba;
  gdk_rgba_parse(&rgba, "#808080");
  GdkRGBA *stored = g_object_get_data(G_OBJECT(w), "da-swatch-color");
  if (stored != NULL) {
    rgba = *stored;
  }

  GtkAllocation a;
  gtk_widget_get_allocation(w, &a);
  gdk_cairo_set_source_rgba(cr, &rgba);
  cairo_rectangle(cr, 0.5, 0.5, (gdouble)a.width - 1.0, (gdouble)a.height - 1.0);
  cairo_fill_preserve(cr);
  cairo_set_source_rgb(cr, 0.45, 0.45, 0.45);
  cairo_set_line_width(cr, 1.0);
  cairo_stroke(cr);
  return FALSE;
}

static void da_mime_attach_color_swatch(GtkWidget *btn) {
  if (btn == NULL || gtk_bin_get_child(GTK_BIN(btn)) != NULL) {
    return;
  }
  GtkWidget *da = gtk_drawing_area_new();
  gtk_widget_set_size_request(da, 32, 22);
  g_signal_connect(da, "draw", G_CALLBACK(da_mime_color_swatch_draw), NULL);
  gtk_container_add(GTK_CONTAINER(btn), da);
  gtk_widget_show(da);
}

static void da_mime_color_preview_update(GtkWidget *btn, const GdkRGBA *rgba) {
  if (btn == NULL || rgba == NULL) {
    return;
  }
  GtkWidget *da = gtk_bin_get_child(GTK_BIN(btn));
  if (da == NULL || !GTK_IS_DRAWING_AREA(da)) {
    return;
  }
  GdkRGBA *copy = g_new(GdkRGBA, 1);
  *copy = *rgba;
  g_object_set_data_full(G_OBJECT(da), "da-swatch-color", copy, g_free);
  gtk_widget_queue_draw(da);
}

static void da_normalize_color_hex_in_place(DaIniMimeCategory *c) {
  if (c->color_hex == NULL || c->color_hex[0] == '\0') {
    return;
  }
  GdkRGBA rgba;
  if (!gdk_rgba_parse(&rgba, c->color_hex)) {
    return;
  }
  gchar *h = da_hex_from_rgba(&rgba);
  g_free(c->color_hex);
  c->color_hex = h;
}

static void da_normalize_name_in_place(DaIniMimeCategory *c) {
  if (c->name == NULL) {
    c->name = g_strdup("");
    return;
  }
  g_strstrip(c->name);
}

static void da_normalize_patterns_in_place(gchar **p) {
  if (p == NULL || *p == NULL) {
    return;
  }
  gchar **lines = g_strsplit(*p, "\n", -1);
  GString *acc = g_string_new(NULL);
  for (gchar **l = lines; *l != NULL; l++) {
    g_strstrip(*l);
    if (**l == '\0') {
      continue;
    }
    if (acc->len > 0) {
      g_string_append_c(acc, '\n');
    }
    g_string_append(acc, *l);
  }
  g_strfreev(lines);
  g_free(*p);
  *p = g_string_free(acc, FALSE);
}

static gboolean da_validate_one_pattern(const gchar *line, GString *err) {
  if (strchr(line, '/') != NULL || strchr(line, '\\') != NULL || strchr(line, ':') != NULL) {
    g_string_printf(err, "Pattern must not contain path separators: %s", line);
    return FALSE;
  }
  for (const gchar *p = line; *p != '\0'; p++) {
    gchar ch = *p;
    if (!((ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9') || ch == '.' ||
          ch == '*' || ch == '_' || ch == '-' || ch == '?')) {
      g_string_printf(err, "Invalid character in pattern: %s", line);
      return FALSE;
    }
  }
  gboolean looks = FALSE;
  if (g_str_has_prefix(line, "*.")) {
    looks = strlen(line) > 2;
  } else if (strchr(line, '.') != NULL) {
    looks = TRUE;
  } else {
    /* Extension-less filename (e.g. Makefile, README) for case-sensitive matching. */
    size_t n = strlen(line);
    if (n >= 2 && n < 256) {
      looks = TRUE;
      for (size_t i = 0; i < n && looks; i++) {
        gchar ch = line[i];
        if (!((ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9') || ch == '_' ||
              ch == '-')) {
          looks = FALSE;
        }
      }
    }
  }
  if (!looks) {
    g_string_printf(err,
                    "Each pattern should be *.ext, include '.' (e.g. .pdf), or be a simple filename (e.g. "
                    "Makefile): %s",
                    line);
    return FALSE;
  }
  return TRUE;
}

static gboolean da_validate_pattern_block(const gchar *text, gboolean require_one_line, GString *err) {
  if (text == NULL || text[0] == '\0') {
    if (require_one_line) {
      g_string_assign(err, "At least one case-insensitive extension pattern is required (one per line).");
      return FALSE;
    }
    return TRUE;
  }
  gchar **lines = g_strsplit(text, "\n", -1);
  gboolean any = FALSE;
  for (gchar **l = lines; *l != NULL; l++) {
    gchar *t = g_strdup(*l);
    g_strstrip(t);
    if (*t == '\0') {
      g_free(t);
      continue;
    }
    if (!da_validate_one_pattern(t, err)) {
      g_free(t);
      g_strfreev(lines);
      return FALSE;
    }
    any = TRUE;
    g_free(t);
  }
  g_strfreev(lines);
  if (require_one_line && !any) {
    g_string_assign(err, "At least one case-insensitive extension pattern is required (one per line).");
    return FALSE;
  }
  return TRUE;
}

static gboolean da_validate_categories(const GPtrArray *cats, GString *err) {
  for (gsize i = 0; i < cats->len; i++) {
    DaIniMimeCategory *c = g_ptr_array_index(cats, i);
    gchar *name = g_strdup(c->name != NULL ? c->name : "");
    g_strstrip(name);
    if (*name == '\0') {
      g_string_printf(err, "Category %zu: name is required.", i + 1);
      g_free(name);
      return FALSE;
    }
    g_free(name);

    GdkRGBA rgba;
    if (c->color_hex == NULL || c->color_hex[0] == '\0' || !gdk_rgba_parse(&rgba, c->color_hex)) {
      g_string_printf(err, "Category %zu: invalid color (use #RGB or #RRGGBB).", i + 1);
      return FALSE;
    }

    GString *sub = g_string_new(NULL);
    if (!da_validate_pattern_block(c->patterns_insensitive, TRUE, sub)) {
      g_string_printf(err, "Category %" G_GSIZE_FORMAT "u: %s", (gsize)(i + 1), sub->str);
      g_string_free(sub, TRUE);
      return FALSE;
    }
    g_string_free(sub, TRUE);

    sub = g_string_new(NULL);
    if (!da_validate_pattern_block(c->patterns_sensitive, FALSE, sub)) {
      g_string_printf(err, "Category %" G_GSIZE_FORMAT "u: %s", (gsize)(i + 1), sub->str);
      g_string_free(sub, TRUE);
      return FALSE;
    }
    g_string_free(sub, TRUE);
  }
  return TRUE;
}

static void da_normalize_all_categories(GPtrArray *cats) {
  for (gsize i = 0; i < cats->len; i++) {
    DaIniMimeCategory *c = g_ptr_array_index(cats, i);
    da_normalize_name_in_place(c);
    da_normalize_color_hex_in_place(c);
    da_normalize_patterns_in_place(&c->patterns_insensitive);
    da_normalize_patterns_in_place(&c->patterns_sensitive);
  }
}

static void da_mime_commit_row(DaSettingsMimeCtx *ctx, gint idx) {
  if (ctx == NULL || idx < 0 || (guint)idx >= ctx->categories->len) {
    return;
  }
  DaIniMimeCategory *c = g_ptr_array_index(ctx->categories, (guint)idx);
  const gchar *nm = "";
  const gchar *hx = "#808080";
  if (ctx->name_entry != NULL) {
    nm = gtk_entry_get_text(ctx->name_entry);
  }
  if (ctx->hex_entry != NULL) {
    hx = gtk_entry_get_text(ctx->hex_entry);
  }
  gchar *ins = (ctx->pat_ins != NULL) ? da_text_view_get_text(ctx->pat_ins) : g_strdup("");
  gchar *pat_s = (ctx->pat_sens != NULL) ? da_text_view_get_text(ctx->pat_sens) : g_strdup("");

  g_free(c->name);
  c->name = g_strdup(nm != NULL ? nm : "");
  g_free(c->color_hex);
  c->color_hex = g_strdup(hx != NULL && hx[0] != '\0' ? hx : "#808080");
  g_free(c->patterns_insensitive);
  c->patterns_insensitive = ins;
  g_free(c->patterns_sensitive);
  c->patterns_sensitive = pat_s;

  if (ctx->category_list != NULL) {
    GtkListBoxRow *row = gtk_list_box_get_row_at_index(ctx->category_list, idx);
    if (row != NULL) {
      GtkWidget *w = gtk_bin_get_child(GTK_BIN(row));
      if (GTK_IS_LABEL(w)) {
        const gchar *disp = (c->name != NULL && c->name[0] != '\0') ? c->name : "(unnamed)";
        gtk_label_set_text(GTK_LABEL(w), disp);
      }
    }
  }
}

static void da_mime_load_row(DaSettingsMimeCtx *ctx, gint idx) {
  if (ctx == NULL || idx < 0 || (guint)idx >= ctx->categories->len) {
    return;
  }
  DaIniMimeCategory *c = g_ptr_array_index(ctx->categories, (guint)idx);
  ctx->frozen = TRUE;
  if (ctx->name_entry != NULL) {
    gtk_entry_set_text(ctx->name_entry, c->name != NULL ? c->name : "");
  }
  const gchar *hx = (c->color_hex != NULL && c->color_hex[0] != '\0') ? c->color_hex : "#808080";
  if (ctx->hex_entry != NULL) {
    gtk_entry_set_text(ctx->hex_entry, hx);
  }
  GdkRGBA rgba;
  if (ctx->color_btn != NULL && gdk_rgba_parse(&rgba, hx)) {
    da_mime_color_preview_update(ctx->color_btn, &rgba);
  }
  if (ctx->pat_ins != NULL) {
    da_text_view_set_text(ctx->pat_ins, c->patterns_insensitive);
  }
  if (ctx->pat_sens != NULL) {
    da_text_view_set_text(ctx->pat_sens, c->patterns_sensitive);
  }
  ctx->frozen = FALSE;
  da_mime_set_details_sensitive(ctx, TRUE);
}

static void da_mime_clear_fields(DaSettingsMimeCtx *ctx) {
  if (ctx == NULL) {
    return;
  }
  ctx->frozen = TRUE;
  if (ctx->name_entry != NULL) {
    gtk_entry_set_text(ctx->name_entry, "");
  }
  if (ctx->hex_entry != NULL) {
    gtk_entry_set_text(ctx->hex_entry, "#808080");
  }
  GdkRGBA rgba;
  gdk_rgba_parse(&rgba, "#808080");
  if (ctx->color_btn != NULL) {
    da_mime_color_preview_update(ctx->color_btn, &rgba);
  }
  if (ctx->pat_ins != NULL) {
    da_text_view_set_text(ctx->pat_ins, "");
  }
  if (ctx->pat_sens != NULL) {
    da_text_view_set_text(ctx->pat_sens, "");
  }
  ctx->frozen = FALSE;
  da_mime_set_details_sensitive(ctx, FALSE);
}

static void da_mime_rebuild_list(DaSettingsMimeCtx *ctx, gint select_idx) {
  if (ctx == NULL || ctx->category_list == NULL) {
    return;
  }
  ctx->frozen = TRUE;

  GList *ch = gtk_container_get_children(GTK_CONTAINER(ctx->category_list));
  for (GList *l = ch; l != NULL; l = l->next) {
    gtk_widget_destroy(GTK_WIDGET(l->data));
  }
  g_list_free(ch);

  for (gsize i = 0; i < ctx->categories->len; i++) {
    DaIniMimeCategory *cat = g_ptr_array_index(ctx->categories, i);
    const gchar *nm = (cat->name != NULL && cat->name[0] != '\0') ? cat->name : "(unnamed)";
    GtkWidget *lbl = gtk_label_new(nm);
    gtk_label_set_ellipsize(GTK_LABEL(lbl), PANGO_ELLIPSIZE_END);
    gtk_widget_set_halign(lbl, GTK_ALIGN_START);
    GtkWidget *row = gtk_list_box_row_new();
    gtk_container_add(GTK_CONTAINER(row), lbl);
    g_object_set_data(G_OBJECT(row), "da-idx", (gpointer)(guintptr)i);
    gtk_widget_show_all(row);
    gtk_list_box_insert(ctx->category_list, row, -1);
  }

  if (ctx->categories->len > 0) {
    gint ix = select_idx;
    if (ix < 0) {
      ix = 0;
    }
    if (ix >= (gint)ctx->categories->len) {
      ix = (gint)ctx->categories->len - 1;
    }
    GtkListBoxRow *r = gtk_list_box_get_row_at_index(ctx->category_list, ix);
    if (r != NULL) {
      gtk_list_box_select_row(ctx->category_list, r);
    }
    ctx->selected_idx = ix;
    da_mime_load_row(ctx, ix);
  } else {
    ctx->selected_idx = -1;
    gtk_list_box_unselect_all(ctx->category_list);
    da_mime_clear_fields(ctx);
  }

  ctx->frozen = FALSE;
}

/** Returns newly allocated trimmed name, or NULL if cancelled / empty name. */
static gchar *da_mime_prompt_new_category_name(GtkWindow *parent) {
  GtkWidget *dlg = gtk_dialog_new_with_buttons("New category", parent,
                                               (GtkDialogFlags)(GTK_DIALOG_MODAL | GTK_DIALOG_DESTROY_WITH_PARENT),
                                               "_Cancel", GTK_RESPONSE_CANCEL, "_Add", GTK_RESPONSE_ACCEPT, NULL);
  gtk_window_set_resizable(GTK_WINDOW(dlg), FALSE);
  GtkWidget *area = gtk_dialog_get_content_area(GTK_DIALOG(dlg));
  gtk_container_set_border_width(GTK_CONTAINER(area), 10);
  GtkWidget *lbl = gtk_label_new("Category name:");
  gtk_widget_set_halign(lbl, GTK_ALIGN_START);
  GtkWidget *entry = gtk_entry_new();
  gtk_entry_set_activates_default(GTK_ENTRY(entry), TRUE);
  gtk_box_pack_start(GTK_BOX(area), lbl, FALSE, FALSE, 0);
  gtk_box_pack_start(GTK_BOX(area), entry, TRUE, TRUE, 6);
  gtk_widget_show_all(area);
  gtk_dialog_set_default_response(GTK_DIALOG(dlg), GTK_RESPONSE_ACCEPT);

  gchar *result = NULL;
  for (;;) {
    gint resp = gtk_dialog_run(GTK_DIALOG(dlg));
    if (resp != GTK_RESPONSE_ACCEPT) {
      break;
    }
    const gchar *t = gtk_entry_get_text(GTK_ENTRY(entry));
    gchar *trim = g_strdup(t != NULL ? t : "");
    g_strstrip(trim);
    if (*trim == '\0') {
      GtkWidget *warn = gtk_message_dialog_new(
          GTK_WINDOW(dlg), GTK_DIALOG_MODAL, GTK_MESSAGE_WARNING, GTK_BUTTONS_OK,
          "%s", "Please enter a category name, or click Cancel.");
      gtk_dialog_run(GTK_DIALOG(warn));
      gtk_widget_destroy(warn);
      g_free(trim);
      continue;
    }
    result = trim;
    break;
  }
  gtk_widget_destroy(dlg);
  return result;
}

static void on_category_row_selected(GtkListBox *box, GtkListBoxRow *row, gpointer user_data) {
  (void)box;
  DaSettingsMimeCtx *ctx = (DaSettingsMimeCtx *)user_data;
  if (ctx->frozen) {
    return;
  }
  gint new_idx = -1;
  if (row != NULL) {
    new_idx = (gint)(guintptr)g_object_get_data(G_OBJECT(row), "da-idx");
  }

  if (ctx->selected_idx >= 0 && ctx->selected_idx != new_idx) {
    da_mime_commit_row(ctx, ctx->selected_idx);
  }
  ctx->selected_idx = new_idx;
  if (new_idx >= 0) {
    da_mime_load_row(ctx, new_idx);
  } else {
    da_mime_clear_fields(ctx);
  }
}

static void on_category_add_clicked(GtkButton *btn, gpointer user_data) {
  (void)btn;
  DaSettingsMimeCtx *ctx = (DaSettingsMimeCtx *)user_data;
  if (ctx->settings_window == NULL) {
    return;
  }
  gchar *new_name = da_mime_prompt_new_category_name(ctx->settings_window);
  if (new_name == NULL) {
    return;
  }
  if (ctx->selected_idx >= 0) {
    da_mime_commit_row(ctx, ctx->selected_idx);
  }
  DaIniMimeCategory *c = g_new0(DaIniMimeCategory, 1);
  c->name = new_name;
  c->color_hex = g_strdup("#808080");
  c->patterns_insensitive = g_strdup("");
  c->patterns_sensitive = g_strdup("");
  g_ptr_array_add(ctx->categories, c);
  da_mime_rebuild_list(ctx, (gint)ctx->categories->len - 1);
}

static void on_category_remove_clicked(GtkButton *btn, gpointer user_data) {
  (void)btn;
  DaSettingsMimeCtx *ctx = (DaSettingsMimeCtx *)user_data;
  gint idx = ctx->selected_idx;
  if (idx < 0 || (guint)idx >= ctx->categories->len) {
    return;
  }
  da_mime_commit_row(ctx, idx);
  g_ptr_array_remove_index(ctx->categories, (guint)idx);
  gint next_sel = idx;
  if (next_sel >= (gint)ctx->categories->len) {
    next_sel = (gint)ctx->categories->len - 1;
  }
  da_mime_rebuild_list(ctx, next_sel);
}

static void on_category_color_btn_clicked(GtkButton *btn, gpointer user_data) {
  DaSettingsMimeCtx *ctx = (DaSettingsMimeCtx *)user_data;
  if (ctx->frozen || ctx->hex_entry == NULL) {
    return;
  }
  GdkRGBA rgba;
  const gchar *hx = gtk_entry_get_text(ctx->hex_entry);
  if (hx == NULL || hx[0] == '\0' || !gdk_rgba_parse(&rgba, hx)) {
    gdk_rgba_parse(&rgba, "#808080");
  }

  GtkWindow *parent = ctx->settings_window;
  if (parent == NULL && ctx->color_btn != NULL) {
    GtkWidget *top = gtk_widget_get_toplevel(ctx->color_btn);
    if (top != NULL && GTK_IS_WINDOW(top)) {
      parent = GTK_WINDOW(top);
    }
  }

  GdkRGBA initial = rgba;
  GdkRGBA original = rgba;
  if (da_color_picker_dialog_run(parent, &initial, &original, &rgba)) {
    gchar *hex = da_hex_from_rgba(&rgba);
    ctx->frozen = TRUE;
    gtk_entry_set_text(ctx->hex_entry, hex);
    ctx->frozen = FALSE;
    g_free(hex);
    da_mime_color_preview_update(GTK_WIDGET(btn), &rgba);
  }
}

static void on_hex_entry_changed(GtkEditable *ed, gpointer user_data) {
  (void)ed;
  DaSettingsMimeCtx *ctx = (DaSettingsMimeCtx *)user_data;
  if (ctx->frozen) {
    return;
  }
  const gchar *t = gtk_entry_get_text(ctx->hex_entry);
  GdkRGBA rgba;
  if (ctx->color_btn != NULL && t != NULL && gdk_rgba_parse(&rgba, t)) {
    ctx->frozen = TRUE;
    da_mime_color_preview_update(ctx->color_btn, &rgba);
    ctx->frozen = FALSE;
  }
}

static void on_name_entry_changed(GtkEditable *ed, gpointer user_data) {
  (void)ed;
  DaSettingsMimeCtx *ctx = (DaSettingsMimeCtx *)user_data;
  if (ctx->frozen || ctx->selected_idx < 0 || ctx->name_entry == NULL || ctx->category_list == NULL) {
    return;
  }
  GtkListBoxRow *row = gtk_list_box_get_row_at_index(ctx->category_list, ctx->selected_idx);
  if (row == NULL) {
    return;
  }
  GtkWidget *w = gtk_bin_get_child(GTK_BIN(row));
  if (!GTK_IS_LABEL(w)) {
    return;
  }
  const gchar *t = gtk_entry_get_text(ctx->name_entry);
  gtk_label_set_text(GTK_LABEL(w), (t != NULL && t[0] != '\0') ? t : "(unnamed)");
}

DaSettingsMimeCtx *da_settings_mime_tab_bind(GtkBuilder *builder) {
  if (builder == NULL) {
    return NULL;
  }
  DaSettingsMimeCtx *ctx = g_new0(DaSettingsMimeCtx, 1);
  ctx->selected_idx = -1;
  ctx->categories = da_ini_mime_categories_load();

  GObject *o = gtk_builder_get_object(builder, "category_list");
  ctx->category_list = (o != NULL && GTK_IS_LIST_BOX(o)) ? GTK_LIST_BOX(o) : NULL;
  o = gtk_builder_get_object(builder, "category_name_text");
  ctx->name_entry = (o != NULL && GTK_IS_ENTRY(o)) ? GTK_ENTRY(o) : NULL;
  o = gtk_builder_get_object(builder, "category_color_btn");
  ctx->color_btn = (o != NULL && GTK_IS_BUTTON(o)) ? GTK_WIDGET(o) : NULL;
  o = gtk_builder_get_object(builder, "category_color_hex_txt");
  ctx->hex_entry = (o != NULL && GTK_IS_ENTRY(o)) ? GTK_ENTRY(o) : NULL;
  o = gtk_builder_get_object(builder, "patterns_insensitive_text");
  ctx->pat_ins = (o != NULL && GTK_IS_TEXT_VIEW(o)) ? GTK_TEXT_VIEW(o) : NULL;
  o = gtk_builder_get_object(builder, "patterns_sensitive_text");
  ctx->pat_sens = (o != NULL && GTK_IS_TEXT_VIEW(o)) ? GTK_TEXT_VIEW(o) : NULL;
  o = gtk_builder_get_object(builder, "patterns_sensitive_label");
  ctx->sensitive_label = (o != NULL && GTK_IS_WIDGET(o)) ? GTK_WIDGET(o) : NULL;
  o = gtk_builder_get_object(builder, "patterns_sensitive_scrolled");
  ctx->sensitive_scrolled = (o != NULL && GTK_IS_WIDGET(o)) ? GTK_WIDGET(o) : NULL;

  o = gtk_builder_get_object(builder, "settings_dialog");
  ctx->settings_window = (o != NULL && GTK_IS_WINDOW(o)) ? GTK_WINDOW(o) : NULL;
  if (ctx->settings_window == NULL && ctx->category_list != NULL) {
    GtkWidget *top = gtk_widget_get_toplevel(GTK_WIDGET(ctx->category_list));
    if (top != NULL && GTK_IS_WINDOW(top)) {
      ctx->settings_window = GTK_WINDOW(top);
    }
  }

#if defined(G_OS_WIN32)
  if (ctx->sensitive_label != NULL) {
    gtk_widget_hide(ctx->sensitive_label);
  }
  if (ctx->sensitive_scrolled != NULL) {
    gtk_widget_hide(ctx->sensitive_scrolled);
  }
#endif

  if (ctx->category_list != NULL) {
    g_signal_connect(ctx->category_list, "row-selected", G_CALLBACK(on_category_row_selected), ctx);
  }
  GtkWidget *add_btn = GTK_WIDGET(gtk_builder_get_object(builder, "category_add_btn"));
  if (add_btn != NULL) {
    g_signal_connect(add_btn, "clicked", G_CALLBACK(on_category_add_clicked), ctx);
  }
  GtkWidget *rm_btn = GTK_WIDGET(gtk_builder_get_object(builder, "category_remove_btn"));
  if (rm_btn != NULL) {
    g_signal_connect(rm_btn, "clicked", G_CALLBACK(on_category_remove_clicked), ctx);
  }
  if (ctx->color_btn != NULL) {
    da_mime_attach_color_swatch(ctx->color_btn);
    g_signal_connect(ctx->color_btn, "clicked", G_CALLBACK(on_category_color_btn_clicked), ctx);
  }
  if (ctx->hex_entry != NULL) {
    g_signal_connect(ctx->hex_entry, "changed", G_CALLBACK(on_hex_entry_changed), ctx);
  }
  if (ctx->name_entry != NULL) {
    g_signal_connect(ctx->name_entry, "changed", G_CALLBACK(on_name_entry_changed), ctx);
  }

  da_mime_rebuild_list(ctx, 0);
  return ctx;
}

void da_settings_mime_tab_free(DaSettingsMimeCtx *ctx) {
  if (ctx == NULL) {
    return;
  }
  if (ctx->categories != NULL) {
    g_ptr_array_unref(ctx->categories);
  }
  g_free(ctx);
}

gboolean da_settings_mime_tab_save(DaSettingsMimeCtx *ctx, GtkWindow *parent) {
  if (ctx == NULL) {
    return TRUE;
  }
  if (ctx->selected_idx >= 0) {
    da_mime_commit_row(ctx, ctx->selected_idx);
  }

  GString *err = g_string_new(NULL);
  if (!da_validate_categories(ctx->categories, err)) {
    GtkWidget *dlg = gtk_message_dialog_new(parent, GTK_DIALOG_MODAL, GTK_MESSAGE_ERROR, GTK_BUTTONS_OK, "%s",
                                              err->str);
    gtk_dialog_run(GTK_DIALOG(dlg));
    gtk_widget_destroy(dlg);
    g_string_free(err, TRUE);
    return FALSE;
  }
  g_string_free(err, TRUE);

  da_normalize_all_categories(ctx->categories);
  da_ini_mime_categories_save(ctx->categories);
  if (ctx->categories->len > 0) {
    gint keep = ctx->selected_idx;
    if (keep < 0) {
      keep = 0;
    }
    if (keep >= (gint)ctx->categories->len) {
      keep = (gint)ctx->categories->len - 1;
    }
    da_mime_rebuild_list(ctx, keep);
  }
  return TRUE;
}
