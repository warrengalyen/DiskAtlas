#include <math.h>
#include <stdlib.h>
#include <string.h>

#include <cairo.h>
#include <glib.h>
#include <gtk/gtk.h>
#include <pango/pangocairo.h>

#include "diskatlas.h"
#include "format_text.h"
#include "treemap_widget.h"

typedef struct TreemapNode TreemapNode;
struct TreemapNode {
  char *name;
  gboolean is_file;
  size_t scan_ix;
  uint64_t leaf_bytes;
  GPtrArray *children;
  uint64_t agg;
};

typedef struct {
  double x, y, w, h;
  char *text;
} TreemapDirLabel;

struct _TreemapWidget {
  GtkDrawingArea parent_instance;

  char *root_utf8;
  const file_node_t *nodes;
  size_t node_count;

  TreemapNode *tree_root;

  treemap_rect_t *rects;
  size_t rect_count;
  char **rect_labels;

  TreemapDirLabel *dir_labels;
  size_t dir_label_count;

  int hovered_index;
  int selected_index;

  int alloc_w;
  int alloc_h;
  gboolean layout_ok;

  void (*on_hover)(GtkWidget *, gint64, gpointer);
  gpointer on_hover_data;
  void (*on_selected)(GtkWidget *, gint64, gpointer);
  gpointer on_selected_data;

  PangoLayout *pango_layout;
};

G_DEFINE_TYPE(TreemapWidget, treemap_widget, GTK_TYPE_DRAWING_AREA)

static void treemap_node_free(TreemapNode *n) {
  size_t i;
  if (n == NULL) {
    return;
  }
  g_free(n->name);
  if (n->children != NULL) {
    for (i = 0; i < n->children->len; i++) {
      treemap_node_free((TreemapNode *)g_ptr_array_index(n->children, i));
    }
    g_ptr_array_free(n->children, TRUE);
  }
  g_free(n);
}

static void treemap_free_dir_labels(TreemapWidget *self) {
  size_t i;
  if (self->dir_labels != NULL) {
    for (i = 0; i < self->dir_label_count; i++) {
      g_free(self->dir_labels[i].text);
    }
    g_free(self->dir_labels);
    self->dir_labels = NULL;
    self->dir_label_count = 0;
  }
}

static void treemap_clear_layout_buffers(TreemapWidget *self) {
  size_t i;
  size_t n = self->rect_count;
  g_free(self->rects);
  self->rects = NULL;
  self->rect_count = 0;
  if (self->rect_labels != NULL) {
    for (i = 0; i < n; i++) {
      g_free(self->rect_labels[i]);
    }
    g_free(self->rect_labels);
    self->rect_labels = NULL;
  }
  treemap_free_dir_labels(self);
}

static void treemap_clear_tree(TreemapWidget *self) {
  treemap_node_free(self->tree_root);
  self->tree_root = NULL;
}

static void path_to_forward_slashes(char *p) {
  for (; p != NULL && *p != '\0'; p++) {
    if (*p == '\\') {
      *p = '/';
    }
  }
}

static void strip_trailing_slashes(char *p) {
  size_t n;
  if (p == NULL) {
    return;
  }
  n = strlen(p);
  while (n > 0 && (p[n - 1] == '/' || p[n - 1] == '\\')) {
    p[--n] = '\0';
  }
}

static gboolean path_has_prefix(const char *path, const char *root, size_t root_len) {
  if (root_len == 0) {
    return TRUE;
  }
#ifdef G_OS_WIN32
  if (g_ascii_strncasecmp(path, root, (gint)root_len) != 0) {
    return FALSE;
  }
#else
  if (strncmp(path, root, root_len) != 0) {
    return FALSE;
  }
#endif
  return path[root_len] == '\0' || path[root_len] == '/' || path[root_len] == '\\';
}

/** Returns relative UTF-8 path without leading slashes; free with g_free. NULL if not under root. */
static gchar *rel_path_under_root(const char *full_utf8, const char *root_utf8) {
  char *norm_full;
  char *norm_root;
  size_t lr;
  gchar *rel;

  if (full_utf8 == NULL || root_utf8 == NULL || root_utf8[0] == '\0') {
    return NULL;
  }
  norm_full = g_strdup(full_utf8);
  norm_root = g_strdup(root_utf8);
  path_to_forward_slashes(norm_full);
  path_to_forward_slashes(norm_root);
  strip_trailing_slashes(norm_full);
  strip_trailing_slashes(norm_root);
  lr = strlen(norm_root);
  if (!path_has_prefix(norm_full, norm_root, lr)) {
    g_free(norm_full);
    g_free(norm_root);
    return NULL;
  }
  rel = g_strdup(norm_full + lr);
  while (rel[0] == '/' || rel[0] == '\\') {
    memmove(rel, rel + 1, strlen(rel));
  }
  g_free(norm_full);
  g_free(norm_root);
  return rel;
}

static TreemapNode *treemap_node_new_dir(const char *name) {
  TreemapNode *n = g_new0(TreemapNode, 1);
  n->name = g_strdup(name);
  n->is_file = FALSE;
  n->scan_ix = (size_t)-1;
  n->children = g_ptr_array_new();
  return n;
}

static TreemapNode *treemap_node_new_file(const char *name, size_t scan_ix, uint64_t sz) {
  TreemapNode *n = g_new0(TreemapNode, 1);
  n->name = g_strdup(name);
  n->is_file = TRUE;
  n->scan_ix = scan_ix;
  n->leaf_bytes = sz;
  n->agg = sz;
  return n;
}

static TreemapNode *treemap_node_find_child_dir(TreemapNode *parent, const char *seg) {
  size_t i;
  for (i = 0; i < parent->children->len; i++) {
    TreemapNode *c = (TreemapNode *)g_ptr_array_index(parent->children, i);
    if (!c->is_file && strcmp(c->name, seg) == 0) {
      return c;
    }
  }
  return NULL;
}

static void treemap_node_add_child(TreemapNode *parent, TreemapNode *child) {
  g_ptr_array_add(parent->children, child);
}

static uint64_t treemap_node_compute_agg(TreemapNode *n) {
  size_t i;
  uint64_t s = 0;
  if (n->is_file) {
    n->agg = n->leaf_bytes;
    return n->agg;
  }
  for (i = 0; i < n->children->len; i++) {
    TreemapNode *c = (TreemapNode *)g_ptr_array_index(n->children, i);
    s += treemap_node_compute_agg(c);
  }
  n->agg = s;
  return n->agg;
}

static TreemapNode *treemap_build_tree(const char *root_utf8, const file_node_t *nodes, size_t count) {
  TreemapNode *root;
  char *root_norm;
  size_t ni;

  root = treemap_node_new_dir("");
  root_norm = g_strdup(root_utf8 != NULL ? root_utf8 : "");
  path_to_forward_slashes(root_norm);
  strip_trailing_slashes(root_norm);

  for (ni = 0; ni < count; ni++) {
    const file_node_t *fn = &nodes[ni];
    uint32_t kind = fn->attributes & DISKATLAS_NODE_KIND_MASK;
    gchar *rel;
    gchar **parts;
    int nseg;
    TreemapNode *walk;
    int si;

    if (fn->path == NULL) {
      continue;
    }
    rel = rel_path_under_root(fn->path, root_norm);
    if (rel == NULL || rel[0] == '\0') {
      g_free(rel);
      continue;
    }

    parts = g_strsplit(rel, "/", -1);
    nseg = 0;
    for (; parts[nseg] != NULL; nseg++) {
    }

    if (nseg <= 0) {
      g_strfreev(parts);
      g_free(rel);
      continue;
    }

    walk = root;
    for (si = 0; si < nseg - 1; si++) {
      const char *seg = parts[si];
      TreemapNode *ch;
      if (seg[0] == '\0') {
        continue;
      }
      ch = treemap_node_find_child_dir(walk, seg);
      if (ch == NULL) {
        ch = treemap_node_new_dir(seg);
        treemap_node_add_child(walk, ch);
      }
      walk = ch;
    }

    {
      const char *leaf = parts[nseg - 1];
      if (leaf[0] == '\0') {
        g_strfreev(parts);
        g_free(rel);
        continue;
      }

      if (kind == DISKATLAS_NODE_KIND_FILE || kind == DISKATLAS_NODE_KIND_SYMLINK ||
          kind == DISKATLAS_NODE_KIND_UNKNOWN) {
        TreemapNode *f = treemap_node_new_file(leaf, ni, fn->size_bytes);
        treemap_node_add_child(walk, f);
      } else if (kind == DISKATLAS_NODE_KIND_DIR) {
        TreemapNode *ch = treemap_node_find_child_dir(walk, leaf);
        if (ch == NULL) {
          ch = treemap_node_new_dir(leaf);
          ch->scan_ix = ni;
          treemap_node_add_child(walk, ch);
        } else if (ch->scan_ix == (size_t)-1) {
          ch->scan_ix = ni;
        }
      }
    }

    g_strfreev(parts);
    g_free(rel);
  }

  g_free(root_norm);
  (void)treemap_node_compute_agg(root);
  return root;
}

typedef struct {
  treemap_rect_t *rects;
  size_t n_rects;
  size_t cap_rects;
  char **rect_labels;
  TreemapDirLabel *dir_labs;
  size_t n_dir;
  size_t cap_dir;
} LayoutOut;

static int layout_push_rect(LayoutOut *out, double x, double y, double w, double h, size_t scan_ix,
                            const char *label_optional) {
  treemap_rect_t *r;
  if (out->n_rects >= out->cap_rects) {
    size_t nc = out->cap_rects ? out->cap_rects * 2u : 4096u;
    treemap_rect_t *nr = g_renew(treemap_rect_t, out->rects, nc);
    char **nl = g_renew(char *, out->rect_labels, nc);
    if (nr == NULL || nl == NULL) {
      return -1;
    }
    out->rects = nr;
    out->rect_labels = nl;
    out->cap_rects = nc;
  }
  r = &out->rects[out->n_rects];
  r->x = x;
  r->y = y;
  r->w = w;
  r->h = h;
  r->node_index = scan_ix;
  out->rect_labels[out->n_rects] = NULL;
  if (label_optional != NULL && w > 60.0 && h > 18.0) {
    out->rect_labels[out->n_rects] = g_strdup(label_optional);
  }
  out->n_rects++;
  return 0;
}

static int layout_push_dir_lab(LayoutOut *out, double x, double y, double w, double h, const char *text) {
  TreemapDirLabel *L;
  if (!(w > 60.0 && h > 18.0)) {
    return 0;
  }
  if (out->n_dir >= out->cap_dir) {
    size_t nc = out->cap_dir ? out->cap_dir * 2u : 64u;
    TreemapDirLabel *a = g_renew(TreemapDirLabel, out->dir_labs, nc);
    if (a == NULL) {
      return -1;
    }
    out->dir_labs = a;
    out->cap_dir = nc;
  }
  L = &out->dir_labs[out->n_dir++];
  L->x = x;
  L->y = y;
  L->w = w;
  L->h = h;
  L->text = g_strdup(text);
  return 0;
}

static int cmp_tnode_agg_desc(const void *a, const void *b) {
  TreemapNode *const *pa = (TreemapNode *const *)a;
  TreemapNode *const *pb = (TreemapNode *const *)b;
  uint64_t ca = (*pa)->agg;
  uint64_t cb = (*pb)->agg;
  if (ca > cb) {
    return -1;
  }
  if (ca < cb) {
    return 1;
  }
  return strcmp((*pa)->name, (*pb)->name);
}

static void layout_slice_dice(LayoutOut *out, TreemapNode *n, double x, double y, double w, double h) {
  size_t i;
  size_t nch;
  TreemapNode **order;
  uint64_t total;
  gboolean horizontal;
  double pos;
  gboolean is_dir_container;

  if (w < 1e-9 || h < 1e-9) {
    return;
  }

  is_dir_container = !n->is_file && n->children != NULL && n->children->len > 0;

  if (n->is_file) {
    (void)layout_push_rect(out, x, y, w, h, n->scan_ix, n->name);
    return;
  }

  if (!is_dir_container) {
    if (!n->is_file && n->scan_ix != (size_t)-1) {
      (void)layout_push_rect(out, x, y, w, h, n->scan_ix, n->name);
    }
    return;
  }

  nch = n->children->len;
  order = g_new(TreemapNode *, nch);
  for (i = 0; i < nch; i++) {
    order[i] = (TreemapNode *)g_ptr_array_index(n->children, i);
  }
  qsort(order, nch, sizeof(TreemapNode *), cmp_tnode_agg_desc);

  total = 0;
  for (i = 0; i < nch; i++) {
    total += order[i]->agg;
  }
  if (total == 0) {
    g_free(order);
    return;
  }

  if (n->name[0] != '\0') {
    char szb[64];
    gchar *lab;
    da_format_bytes(n->agg, szb, sizeof szb);
    lab = g_strdup_printf("%s\\ (%s)", n->name, szb);
    (void)layout_push_dir_lab(out, x, y, w, h, lab);
    g_free(lab);
  }

  horizontal = w >= h;
  pos = 0.0;

  for (i = 0; i < nch; i++) {
    TreemapNode *ch = order[i];
    double frac = (double)ch->agg / (double)total;
    double strip;
    gboolean last = (i + 1 == nch);

    if (horizontal) {
      strip = last ? (w - pos) : w * frac;
      layout_slice_dice(out, ch, x + pos, y, strip, h);
      pos += strip;
    } else {
      strip = last ? (h - pos) : h * frac;
      layout_slice_dice(out, ch, x, y + pos, w, strip);
      pos += strip;
    }
  }

  g_free(order);
}

static void hsv_to_rgb(double h, double s, double v, double *r, double *g, double *b) {
  double c = v * s;
  double x = c * (1.0 - fabs(fmod(h / 60.0, 2.0) - 1.0));
  double m = v - c;
  double rp = 0, gp = 0, bp = 0;
  if (h < 60.0) {
    rp = c;
    gp = x;
  } else if (h < 120.0) {
    rp = x;
    gp = c;
  } else if (h < 180.0) {
    gp = c;
    bp = x;
  } else if (h < 240.0) {
    gp = x;
    bp = c;
  } else if (h < 300.0) {
    rp = x;
    bp = c;
  } else {
    rp = c;
    bp = x;
  }
  *r = rp + m;
  *g = gp + m;
  *b = bp + m;
}

static void color_for_path(const char *path, double *r, double *g, double *b) {
  const char *base;
  const char *dot;
  guint32 h32;
  double hue;

  if (path == NULL) {
    *r = *g = *b = 0.35;
    return;
  }
  base = strrchr(path, '/');
  base = base != NULL ? base + 1 : path;
#ifdef G_OS_WIN32
  {
    const char *bs = strrchr(base, '\\');
    if (bs != NULL) {
      base = bs + 1;
    }
  }
#endif
  dot = strrchr(base, '.');
  if (dot == NULL || dot == base) {
    *r = 0.28;
    *g = 0.28;
    *b = 0.30;
    return;
  }
  {
    char ext_key[40];
    size_t j = 0;
    const char *e = dot;
    while (e[j] != '\0' && j + 1 < sizeof ext_key) {
      ext_key[j] = (char)g_ascii_tolower((guchar)e[j]);
      j++;
    }
    ext_key[j] = '\0';
    h32 = g_str_hash(ext_key);
  }
  hue = (double)(h32 % 360u);
  hsv_to_rgb(hue, 0.88, 0.74, r, g, b);
}

static void treemap_run_layout(TreemapWidget *self, int wid, int hei) {
  LayoutOut out = {0};

  treemap_clear_layout_buffers(self);
  self->layout_ok = FALSE;
  self->hovered_index = -1;

  if (self->tree_root == NULL || wid < 2 || hei < 2) {
    gtk_widget_queue_draw(GTK_WIDGET(self));
    return;
  }

  layout_slice_dice(&out, self->tree_root, 0.0, 0.0, (double)wid, (double)hei);

  self->rects = out.rects;
  self->rect_count = out.n_rects;
  self->rect_labels = out.rect_labels;
  self->dir_labels = out.dir_labs;
  self->dir_label_count = out.n_dir;
  self->alloc_w = wid;
  self->alloc_h = hei;
  self->layout_ok = TRUE;

  gtk_widget_queue_draw(GTK_WIDGET(self));
}

static int treemap_hit_index(const TreemapWidget *self, double px, double py) {
  gint j;
  if (self->rects == NULL || self->rect_count == 0) {
    return -1;
  }
  for (j = (gint)self->rect_count - 1; j >= 0; j--) {
    const treemap_rect_t *r = &self->rects[j];
    if (px >= r->x && px < r->x + r->w && py >= r->y && py < r->y + r->h) {
      return (int)j;
    }
  }
  return -1;
}

static gboolean treemap_draw(GtkWidget *widget, cairo_t *cr) {
  TreemapWidget *self = TREEMAP_WIDGET(widget);
  size_t i;

  cairo_set_source_rgb(cr, 0.06, 0.06, 0.07);
  cairo_paint(cr);

  if (!self->layout_ok || self->rects == NULL || self->nodes == NULL) {
    return FALSE;
  }

  for (i = 0; i < self->rect_count; i++) {
    const treemap_rect_t *R = &self->rects[i];
    const file_node_t *fn;
    double rf, gf, bf;
    double br, bg, bb;
    uint32_t kind;

    if (R->w < 1.0 || R->h < 1.0) {
      continue;
    }
    if (R->node_index >= self->node_count) {
      continue;
    }
    fn = &self->nodes[R->node_index];
    kind = fn->attributes & DISKATLAS_NODE_KIND_MASK;
    if (kind == DISKATLAS_NODE_KIND_DIR) {
      rf = 0.28;
      gf = 0.28;
      bf = 0.30;
    } else {
      color_for_path(fn->path, &rf, &gf, &bf);
    }

    cairo_set_source_rgb(cr, rf, gf, bf);
    cairo_rectangle(cr, R->x, R->y, R->w, R->h);
    cairo_fill(cr);

    br = rf * 0.52;
    bg = gf * 0.52;
    bb = bf * 0.52;
    cairo_set_source_rgb(cr, br, bg, bb);
    cairo_set_line_width(cr, 1.0);
    cairo_rectangle(cr, R->x + 0.25, R->y + 0.25, R->w - 0.5, R->h - 0.5);
    cairo_stroke(cr);
  }

  for (i = 0; i < self->rect_count; i++) {
    const treemap_rect_t *R = &self->rects[i];
    const file_node_t *fn;
    double rf, gf, bf;
    uint32_t kind;
    if (R->w < 1.0 || R->h < 1.0) {
      continue;
    }
    if (R->node_index >= self->node_count) {
      continue;
    }
    if ((gint)i != self->hovered_index && (gint)i != self->selected_index) {
      continue;
    }
    fn = &self->nodes[R->node_index];
    kind = fn->attributes & DISKATLAS_NODE_KIND_MASK;
    if (kind == DISKATLAS_NODE_KIND_DIR) {
      rf = 0.28;
      gf = 0.28;
      bf = 0.30;
    } else {
      color_for_path(fn->path, &rf, &gf, &bf);
    }
    if ((gint)i == self->selected_index) {
      cairo_set_source_rgb(cr, fmin(1.0, rf + 0.22), fmin(1.0, gf + 0.22), fmin(1.0, bf + 0.22));
      cairo_set_line_width(cr, 2.0);
    } else {
      cairo_set_source_rgb(cr, fmin(1.0, rf + 0.14), fmin(1.0, gf + 0.14), fmin(1.0, bf + 0.14));
      cairo_set_line_width(cr, 1.0);
    }
    cairo_rectangle(cr, R->x + 0.25, R->y + 0.25, R->w - 0.5, R->h - 0.5);
    cairo_stroke(cr);
  }

  if (self->pango_layout != NULL) {
    cairo_set_source_rgb(cr, 0.94, 0.94, 0.95);
    for (i = 0; i < self->rect_count; i++) {
      const treemap_rect_t *R = &self->rects[i];
      const char *txt = self->rect_labels != NULL ? self->rect_labels[i] : NULL;
      if (txt == NULL || R->w <= 60.0 || R->h <= 18.0) {
        continue;
      }
      pango_layout_set_width(self->pango_layout, (int)((R->w - 4.0) * PANGO_SCALE));
      pango_layout_set_ellipsize(self->pango_layout, PANGO_ELLIPSIZE_END);
      pango_layout_set_text(self->pango_layout, txt, -1);
      cairo_save(cr);
      cairo_rectangle(cr, R->x, R->y, R->w, R->h);
      cairo_clip(cr);
      cairo_move_to(cr, R->x + 2.0, R->y + 2.0);
      pango_cairo_show_layout(cr, self->pango_layout);
      cairo_restore(cr);
    }
    for (i = 0; i < self->dir_label_count; i++) {
      const TreemapDirLabel *L = &self->dir_labels[i];
      pango_layout_set_width(self->pango_layout, (int)((L->w - 4.0) * PANGO_SCALE));
      pango_layout_set_ellipsize(self->pango_layout, PANGO_ELLIPSIZE_END);
      pango_layout_set_text(self->pango_layout, L->text, -1);
      cairo_save(cr);
      cairo_rectangle(cr, L->x, L->y, L->w, L->h);
      cairo_clip(cr);
      cairo_move_to(cr, L->x + 2.0, L->y + 2.0);
      pango_cairo_show_layout(cr, self->pango_layout);
      cairo_restore(cr);
    }
  }

  return FALSE;
}

static gboolean treemap_motion(GtkWidget *w, GdkEventMotion *ev, gpointer user_data) {
  TreemapWidget *self = TREEMAP_WIDGET(user_data);
  int hit = treemap_hit_index(self, ev->x, ev->y);
  (void)w;
  if (hit != self->hovered_index) {
    self->hovered_index = hit;
    gtk_widget_queue_draw(GTK_WIDGET(self));
    if (self->on_hover != NULL) {
      gint64 ix = (hit < 0) ? -1 : (gint64)self->rects[hit].node_index;
      self->on_hover(GTK_WIDGET(self), ix, self->on_hover_data);
    }
  }
  return FALSE;
}

static gboolean treemap_leave(GtkWidget *w, GdkEventCrossing *ev, gpointer user_data) {
  TreemapWidget *self = TREEMAP_WIDGET(user_data);
  (void)w;
  (void)ev;
  if (self->hovered_index >= 0) {
    self->hovered_index = -1;
    gtk_widget_queue_draw(GTK_WIDGET(self));
    if (self->on_hover != NULL) {
      self->on_hover(GTK_WIDGET(self), -1, self->on_hover_data);
    }
  }
  return FALSE;
}

static gboolean treemap_button_press(GtkWidget *w, GdkEventButton *ev, gpointer user_data) {
  TreemapWidget *self = TREEMAP_WIDGET(user_data);
  int hit;
  (void)w;
  if (ev->button != 1U) {
    return FALSE;
  }
  hit = treemap_hit_index(self, ev->x, ev->y);
  self->selected_index = hit;
  gtk_widget_queue_draw(GTK_WIDGET(self));
  if (self->on_selected != NULL) {
    gint64 ix = (hit < 0) ? -1 : (gint64)self->rects[hit].node_index;
    self->on_selected(GTK_WIDGET(self), ix, self->on_selected_data);
  }
  return FALSE;
}

static void treemap_size_allocate(GtkWidget *widget, GdkRectangle *allocation, gpointer user_data) {
  TreemapWidget *self = TREEMAP_WIDGET(user_data);
  (void)allocation;
  if (allocation->width != self->alloc_w || allocation->height != self->alloc_h || !self->layout_ok) {
    if (self->tree_root != NULL && allocation->width >= 2 && allocation->height >= 2) {
      treemap_run_layout(self, allocation->width, allocation->height);
    }
  }
}

static void treemap_realize(GtkWidget *widget, gpointer user_data) {
  TreemapWidget *self = TREEMAP_WIDGET(user_data);
  if (self->pango_layout == NULL) {
    self->pango_layout = gtk_widget_create_pango_layout(widget, "");
    pango_layout_set_single_paragraph_mode(self->pango_layout, TRUE);
  }
}

static void treemap_unrealize(GtkWidget *widget, gpointer user_data) {
  TreemapWidget *self = TREEMAP_WIDGET(user_data);
  (void)widget;
  g_clear_object(&self->pango_layout);
}

static void treemap_widget_finalize(GObject *object) {
  TreemapWidget *self = TREEMAP_WIDGET(object);
  g_free(self->root_utf8);
  treemap_clear_tree(self);
  treemap_clear_layout_buffers(self);
  g_clear_object(&self->pango_layout);
  G_OBJECT_CLASS(treemap_widget_parent_class)->finalize(object);
}

static void treemap_widget_class_init(TreemapWidgetClass *klass) {
  GObjectClass *gobject_class = G_OBJECT_CLASS(klass);
  GtkWidgetClass *widget_class = GTK_WIDGET_CLASS(klass);
  gobject_class->finalize = treemap_widget_finalize;
  widget_class->draw = treemap_draw;
}

static void treemap_widget_init(TreemapWidget *self) {
  gtk_widget_set_has_window(GTK_WIDGET(self), TRUE);
  gtk_widget_add_events(GTK_WIDGET(self),
                        GDK_POINTER_MOTION_MASK | GDK_BUTTON_PRESS_MASK | GDK_LEAVE_NOTIFY_MASK);
  self->hovered_index = -1;
  self->selected_index = -1;
  self->alloc_w = -1;
  self->alloc_h = -1;
  g_signal_connect(self, "size-allocate", G_CALLBACK(treemap_size_allocate), self);
  g_signal_connect(self, "realize", G_CALLBACK(treemap_realize), self);
  g_signal_connect(self, "unrealize", G_CALLBACK(treemap_unrealize), self);
  g_signal_connect(self, "motion-notify-event", G_CALLBACK(treemap_motion), self);
  g_signal_connect(self, "leave-notify-event", G_CALLBACK(treemap_leave), self);
  g_signal_connect(self, "button-press-event", G_CALLBACK(treemap_button_press), self);
}

GtkWidget *treemap_widget_new(void) {
  return GTK_WIDGET(g_object_new(TREEMAP_TYPE_WIDGET, NULL));
}

void treemap_widget_set_data(TreemapWidget *widget, const char *root_utf8, const file_node_t *nodes,
                             size_t count) {
  GtkAllocation a;

  g_return_if_fail(TREEMAP_IS_WIDGET(widget));

  g_free(widget->root_utf8);
  widget->root_utf8 = g_strdup(root_utf8 != NULL ? root_utf8 : "");
  widget->nodes = nodes;
  widget->node_count = count;

  treemap_clear_layout_buffers(widget);
  treemap_clear_tree(widget);
  widget->layout_ok = FALSE;
  widget->selected_index = -1;
  widget->hovered_index = -1;

  if (nodes != NULL && count > 0 && widget->root_utf8[0] != '\0') {
    widget->tree_root = treemap_build_tree(widget->root_utf8, nodes, count);
  } else {
    widget->tree_root = NULL;
  }

  if (gtk_widget_get_realized(GTK_WIDGET(widget))) {
    gtk_widget_get_allocation(GTK_WIDGET(widget), &a);
    if (a.width >= 2 && a.height >= 2) {
      treemap_run_layout(widget, a.width, a.height);
    }
  } else {
    gtk_widget_queue_resize(GTK_WIDGET(widget));
  }
}

void treemap_widget_set_hover_callback(TreemapWidget *w,
                                       void (*cb)(GtkWidget *widget, gint64 scan_index, gpointer data),
                                       gpointer data) {
  g_return_if_fail(TREEMAP_IS_WIDGET(w));
  w->on_hover = cb;
  w->on_hover_data = data;
}

void treemap_widget_set_selected_callback(TreemapWidget *w,
                                          void (*cb)(GtkWidget *widget, gint64 scan_index, gpointer data),
                                          gpointer data) {
  g_return_if_fail(TREEMAP_IS_WIDGET(w));
  w->on_selected = cb;
  w->on_selected_data = data;
}
