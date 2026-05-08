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

void treemap_widget_append_selected_scan_indices(TreemapWidget *w, GArray *out_nids);
void treemap_widget_add_to_selection_by_scan_index(TreemapWidget *w, gint64 scan_index);

static void treemap_collect_selected_scan_nids_from_rects(const TreemapWidget *w, GArray *out_nids);
static void treemap_persist_sync_from_visual(TreemapWidget *self);
static void treemap_append_unique_nid(GArray *out, size_t nid);

/* ---- visual constants ---------------------------------------------------- */
#define DIR_HEADER_H    16.0  /* pixel height reserved for folder header strip  */
#define MIN_LABEL_W     72.0  /* min rect width  to show any text               */
#define MIN_LABEL_H     13.0  /* min rect height to show any text               */
#define LARGE_W         90.0  /* rect width  threshold for normal-font label    */
#define LARGE_H         26.0  /* rect height threshold for normal-font label    */

/* Hover / selection border appearance */
#define HOVER_BORDER_R  1.00  /* border color (white) */
#define HOVER_BORDER_G  1.00
#define HOVER_BORDER_B  1.00
#define HOVER_BORDER_A  0.80  /* opacity */
#define HOVER_BORDER_W  1.2   /* line width while hovering  (thin)   */
#define SELECT_BORDER_W 2.0   /* line width when selected   (thick)  */

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
  double x, y, w, h; /* header strip bounds                                    */
  double dir_h;       /* total height of the directory group (header+children) */
  size_t scan_ix;     /* scan node index for this directory; (size_t)-1 if unknown */
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
  int hovered_dir_index;    /* index into dir_labels[] whose header is under the cursor  */

  /* Multi-selection: GArray of gint rect/dir indices. */
  GArray *selected_rect_indices; /* gint elements – indices into rects[]      */
  GArray *selected_dir_indices;  /* gint elements – indices into dir_labels[] */
  gint    anchor_rect_index;     /* Shift-click anchor for rects; -1 if none  */

  int alloc_w;
  int alloc_h;
  gboolean layout_ok;

  void (*on_hover)(GtkWidget *, gint64, gpointer);
  gpointer on_hover_data;
  void (*on_selected)(GtkWidget *, gint64, gpointer);
  gpointer on_selected_data;

  /* Logical multi-selection (scan indices) survives set_data / relayout when the same scan buffer is reused. */
  GArray *persist_sel_nids;

  PangoLayout *pango_layout;    /* normal font – large rects            */
  PangoLayout *pango_layout_sm; /* small  font – medium rects           */
  PangoLayout *pango_layout_hdr;/* bold font sized to DIR_HEADER_H – dir headers */
};

G_DEFINE_TYPE(TreemapWidget, treemap_widget, GTK_TYPE_DRAWING_AREA)

/* ---- tree helpers -------------------------------------------------------- */

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

/* ---- path utilities ------------------------------------------------------ */

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

/* ---- tree construction --------------------------------------------------- */

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

  root = treemap_node_new_dir(root_utf8 != NULL ? root_utf8 : "");
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

  /* Find the scan node for the root directory itself so its header strip is
   * interactive (hover path shown in status bar, click selects in tree view). */
  for (ni = 0; ni < count; ni++) {
    const file_node_t *fn = &nodes[ni];
    uint32_t kind = fn->attributes & DISKATLAS_NODE_KIND_MASK;
    if (kind == DISKATLAS_NODE_KIND_DIR && fn->path != NULL) {
      char *p = g_strdup(fn->path);
      path_to_forward_slashes(p);
      strip_trailing_slashes(p);
#ifdef G_OS_WIN32
      if (g_ascii_strcasecmp(p, root_norm) == 0) {
#else
      if (strcmp(p, root_norm) == 0) {
#endif
        root->scan_ix = ni;
        g_free(p);
        break;
      }
      g_free(p);
    }
  }

  g_free(root_norm);
  (void)treemap_node_compute_agg(root);
  return root;
}

/* ---- layout output buffers ----------------------------------------------- */

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
  if (label_optional != NULL && w > MIN_LABEL_W && h > MIN_LABEL_H) {
    out->rect_labels[out->n_rects] = g_strdup(label_optional);
  }
  out->n_rects++;
  return 0;
}

/* Stores a folder header strip.
 * h      = strip height (DIR_HEADER_H).
 * dir_h  = full directory height (header + children area).
 * nix    = scan node index for this directory ((size_t)-1 if unknown). */
static int layout_push_dir_lab(LayoutOut *out, double x, double y, double w, double h,
                               double dir_h, size_t nix, const char *text) {
  TreemapDirLabel *L;
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
  L->dir_h = dir_h;
  L->scan_ix = nix;
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

/* ---- squarified treemap layout (Bruls / KDirStat style) ------------------ */

/*
 * Returns the worst aspect ratio for a candidate row.
 *   sum      – sum of pixel areas in the row
 *   amax     – largest pixel area in the row
 *   amin     – smallest pixel area in the row
 *   side     – length of the strip's fixed dimension (shorter rect side)
 *
 * For a strip of height h = sum/side, each item i has width = area_i/h.
 * Aspect ratio of item i = max(h/(area_i/h),  (area_i/h)/h)
 *                        = max(h²/area_i, area_i/h²).
 * Worst is the max over largest and smallest items.
 */
static double sq_worst(double sum, double amax, double amin, double side) {
  double sw;
  if (side < 1e-12 || amin < 1e-12) {
    return 1e18;
  }
  sw = sum / side;
  if (sw < 1e-12) {
    return 1e18;
  }
  return fmax(sw * sw / amin, amax / (sw * sw));
}

/* Forward declaration for mutual recursion between squarify and dir layout. */
static void layout_squarify_node(LayoutOut *out, TreemapNode *n,
                                  double x, double y, double w, double h);

/*
 * Squarify a pre-sorted (descending) array of items with pre-computed pixel
 * areas into the rectangle (x,y,w,h), consuming the rectangle from top/left.
 */
static void squarify_impl(LayoutOut *out, TreemapNode **items, int n,
                           double *areas, double x, double y, double w, double h) {
  int i;
  double rx, ry, rw, rh;

  i = 0;
  rx = x;
  ry = y;
  rw = w;
  rh = h;

  while (i < n && rw > 1.0 && rh > 1.0) {
    int j;
    int row_end;
    double row_sum, row_max, row_min;
    double strip, pos;
    gboolean horiz;
    double free_dim;

    horiz = (rw >= rh);
    free_dim = horiz ? rw : rh;

    /* Seed the row with item i. */
    row_sum = areas[i];
    row_max = areas[i];
    row_min = areas[i] > 1e-9 ? areas[i] : 1e-9;
    row_end = i + 1;

    /* Greedily extend the row while aspect ratio improves or stays equal. */
    for (j = i + 1; j < n; j++) {
      double ns, nx, nm;
      if (areas[j] < 1e-9) {
        /* Zero-area item – absorb silently without affecting aspect ratio. */
        row_sum += areas[j];
        row_end = j + 1;
        continue;
      }
      ns = row_sum + areas[j];
      nx = fmax(row_max, areas[j]);
      nm = fmin(row_min, areas[j]);
      if (sq_worst(ns, nx, nm, free_dim) <= sq_worst(row_sum, row_max, row_min, free_dim) + 1e-9) {
        row_sum = ns;
        row_max = nx;
        row_min = nm;
        row_end = j + 1;
      } else {
        break;
      }
    }

    strip = (free_dim > 1e-9) ? (row_sum / free_dim) : 0.0;
    if (strip < 1e-9) {
      break;
    }

    /* Place each item in the committed row. */
    pos = 0.0;
    for (j = i; j < row_end; j++) {
      gboolean last = (j + 1 == row_end);
      double free_len = horiz ? rw : rh;
      double item_len = last ? (free_len - pos) : (areas[j] / strip);
      double cx, cy, cw, ch;
      TreemapNode *node;

      if (horiz) {
        cx = rx + pos;
        cy = ry;
        cw = item_len;
        ch = strip;
      } else {
        cx = rx;
        cy = ry + pos;
        cw = strip;
        ch = item_len;
      }
      pos += item_len;

      node = items[j];
      if (node->is_file) {
        (void)layout_push_rect(out, cx, cy, cw, ch, node->scan_ix, node->name);
      } else if (node->children != NULL && node->children->len > 0) {
        layout_squarify_node(out, node, cx, cy, cw, ch);
      } else if (node->scan_ix != (size_t)-1) {
        (void)layout_push_rect(out, cx, cy, cw, ch, node->scan_ix, node->name);
      }
    }

    /* Shrink the remaining rectangle. */
    if (horiz) {
      ry += strip;
      rh -= strip;
    } else {
      rx += strip;
      rw -= strip;
    }
    i = row_end;
  }
}

/*
 * Layout a directory node into (x,y,w,h):
 *  1. If the node has a name and the rect is tall enough, reserve DIR_HEADER_H
 *     pixels at the top for a folder label strip.
 *  2. Squarify the children into the remaining inner area.
 */
static void layout_squarify_node(LayoutOut *out, TreemapNode *n,
                                  double x, double y, double w, double h) {
  double inner_y, inner_h;
  int nch, i;
  TreemapNode **order;
  uint64_t total;
  double total_px;
  double *areas;

  if (w < 1.0 || h < 1.0) {
    return;
  }

  inner_y = y;
  inner_h = h;

  /* Folder header strip – only when named and rect has enough room. */
  if (n->name != NULL && n->name[0] != '\0' && w > MIN_LABEL_W && h > DIR_HEADER_H + 4.0) {
    char szb[64];
    gchar *lab;
    da_format_bytes(n->agg, szb, sizeof szb);
    lab = g_strdup_printf("%s (%s)", n->name, szb);
    (void)layout_push_dir_lab(out, x, y, w, DIR_HEADER_H, h, n->scan_ix, lab);
    g_free(lab);
    inner_y = y + DIR_HEADER_H;
    inner_h = h - DIR_HEADER_H;
  }

  if (n->children == NULL || n->children->len == 0) {
    return;
  }
  if (inner_h < 1.0) {
    return;
  }

  nch = (int)n->children->len;
  order = g_new(TreemapNode *, nch);
  for (i = 0; i < nch; i++) {
    order[i] = (TreemapNode *)g_ptr_array_index(n->children, i);
  }
  qsort(order, nch, sizeof(TreemapNode *), cmp_tnode_agg_desc);

  /* Drop trailing zero-area children (sorted to the back). */
  while (nch > 0 && order[nch - 1]->agg == 0) {
    nch--;
  }

  total = 0;
  for (i = 0; i < nch; i++) {
    total += order[i]->agg;
  }
  if (total == 0) {
    g_free(order);
    return;
  }

  total_px = w * inner_h;
  areas = g_new(double, nch);
  for (i = 0; i < nch; i++) {
    areas[i] = (double)order[i]->agg / (double)total * total_px;
  }

  squarify_impl(out, order, nch, areas, x, inner_y, w, inner_h);

  g_free(areas);
  g_free(order);
}

/* ---- colour helpers ------------------------------------------------------ */

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

/* ---- multi-selection helpers --------------------------------------------- */

static gboolean treemap_rect_is_selected(const TreemapWidget *self, gint idx) {
  guint i;
  if (self->selected_rect_indices == NULL) return FALSE;
  for (i = 0; i < self->selected_rect_indices->len; i++) {
    if (g_array_index(self->selected_rect_indices, gint, i) == idx) return TRUE;
  }
  return FALSE;
}

static gboolean treemap_dir_is_selected(const TreemapWidget *self, gint idx) {
  guint i;
  if (self->selected_dir_indices == NULL) return FALSE;
  for (i = 0; i < self->selected_dir_indices->len; i++) {
    if (g_array_index(self->selected_dir_indices, gint, i) == idx) return TRUE;
  }
  return FALSE;
}

/* Toggle rect index in the selected_rect_indices array. */
static void treemap_rect_toggle(TreemapWidget *self, gint idx) {
  guint i;
  for (i = 0; i < self->selected_rect_indices->len; i++) {
    if (g_array_index(self->selected_rect_indices, gint, i) == idx) {
      g_array_remove_index_fast(self->selected_rect_indices, i);
      return;
    }
  }
  g_array_append_val(self->selected_rect_indices, idx);
}

/* Toggle dir index in the selected_dir_indices array. */
static void treemap_dir_toggle(TreemapWidget *self, gint idx) {
  guint i;
  for (i = 0; i < self->selected_dir_indices->len; i++) {
    if (g_array_index(self->selected_dir_indices, gint, i) == idx) {
      g_array_remove_index_fast(self->selected_dir_indices, i);
      return;
    }
  }
  g_array_append_val(self->selected_dir_indices, idx);
}

/* Fills out_nids from current rect/dir index selection (no persist fallback). */
static void treemap_collect_selected_scan_nids_from_rects(const TreemapWidget *w, GArray *out_nids) {
  guint i;
  if (w->selected_rect_indices != NULL) {
    for (i = 0; i < w->selected_rect_indices->len; i++) {
      gint ri = g_array_index(w->selected_rect_indices, gint, i);
      if (ri >= 0 && (size_t)ri < w->rect_count) {
        size_t nid = w->rects[ri].node_index;
        if (nid < w->node_count) {
          treemap_append_unique_nid(out_nids, nid);
        }
      }
    }
  }
  if (w->selected_dir_indices != NULL) {
    for (i = 0; i < w->selected_dir_indices->len; i++) {
      gint di = g_array_index(w->selected_dir_indices, gint, i);
      if (di >= 0 && (size_t)di < w->dir_label_count) {
        size_t ix = w->dir_labels[di].scan_ix;
        if (ix != (size_t)-1 && ix < w->node_count) {
          treemap_append_unique_nid(out_nids, ix);
        }
      }
    }
  }
}

static void treemap_persist_sync_from_visual(TreemapWidget *self) {
  if (self->persist_sel_nids == NULL) {
    return;
  }
  g_array_set_size(self->persist_sel_nids, 0);
  if (self->layout_ok && self->rects != NULL && self->nodes != NULL) {
    treemap_collect_selected_scan_nids_from_rects(self, self->persist_sel_nids);
  }
}

/* ---- layout entry point -------------------------------------------------- */

static void treemap_run_layout(TreemapWidget *self, int wid, int hei) {
  LayoutOut out = {0};
  GArray *preserve_scan_nids = g_array_new(FALSE, FALSE, sizeof(size_t));
  guint pi;

  /* Snapshot logical selection before tearing down rects. */
  if (self->layout_ok && self->rects != NULL && self->nodes != NULL) {
    treemap_collect_selected_scan_nids_from_rects(self, preserve_scan_nids);
  } else if (self->persist_sel_nids != NULL && self->persist_sel_nids->len > 0 && self->nodes != NULL) {
    for (pi = 0; pi < self->persist_sel_nids->len; pi++) {
      size_t nid = g_array_index(self->persist_sel_nids, size_t, pi);
      if (nid < self->node_count) {
        treemap_append_unique_nid(preserve_scan_nids, nid);
      }
    }
  }

  treemap_clear_layout_buffers(self);
  self->layout_ok = FALSE;
  self->hovered_index     = -1;
  self->hovered_dir_index = -1;
  if (self->selected_rect_indices) g_array_set_size(self->selected_rect_indices, 0);
  if (self->selected_dir_indices)  g_array_set_size(self->selected_dir_indices,  0);
  self->anchor_rect_index = -1;

  if (self->tree_root == NULL || wid < 2 || hei < 2) {
    if (self->persist_sel_nids != NULL) {
      g_array_set_size(self->persist_sel_nids, 0);
      for (pi = 0; pi < preserve_scan_nids->len; pi++) {
        size_t nid = g_array_index(preserve_scan_nids, size_t, pi);
        if (self->nodes != NULL && nid < self->node_count) {
          treemap_append_unique_nid(self->persist_sel_nids, nid);
        }
      }
    }
    g_array_free(preserve_scan_nids, TRUE);
    gtk_widget_queue_draw(GTK_WIDGET(self));
    return;
  }

  layout_squarify_node(&out, self->tree_root, 0.0, 0.0, (double)wid, (double)hei);

  self->rects = out.rects;
  self->rect_count = out.n_rects;
  self->rect_labels = out.rect_labels;
  self->dir_labels = out.dir_labs;
  self->dir_label_count = out.n_dir;
  self->alloc_w = wid;
  self->alloc_h = hei;
  self->layout_ok = TRUE;

  for (pi = 0; pi < preserve_scan_nids->len; pi++) {
    size_t nid = g_array_index(preserve_scan_nids, size_t, pi);
    treemap_widget_add_to_selection_by_scan_index(self, (gint64)nid);
  }
  g_array_free(preserve_scan_nids, TRUE);

  treemap_persist_sync_from_visual(self);
  gtk_widget_queue_draw(GTK_WIDGET(self));
}

/* ---- hit testing --------------------------------------------------------- */

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

/* Returns the index of the dir_labels[] entry whose header strip contains (px,py), else -1. */
static int treemap_hit_dir_header(const TreemapWidget *self, double px, double py) {
  size_t i;
  if (self->dir_labels == NULL || self->dir_label_count == 0) {
    return -1;
  }
  for (i = 0; i < self->dir_label_count; i++) {
    const TreemapDirLabel *L = &self->dir_labels[i];
    if (px >= L->x && px < L->x + L->w && py >= L->y && py < L->y + L->h) {
      return (int)i;
    }
  }
  return -1;
}

/* ---- drawing ------------------------------------------------------------- */

static gboolean treemap_draw(GtkWidget *widget, cairo_t *cr) {
  TreemapWidget *self = TREEMAP_WIDGET(widget);
  size_t i;

  cairo_set_source_rgb(cr, 0.06, 0.06, 0.07);
  cairo_paint(cr);

  if (!self->layout_ok || self->rects == NULL || self->nodes == NULL) {
    return FALSE;
  }

  /* --- Pass 1: fill all file/dir rects with colour + border --- */
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

    br = rf * 0.50;
    bg = gf * 0.50;
    bb = bf * 0.50;
    cairo_set_source_rgb(cr, br, bg, bb);
    cairo_set_line_width(cr, 0.5);
    cairo_rectangle(cr, R->x + 0.25, R->y + 0.25, R->w - 0.5, R->h - 0.5);
    cairo_stroke(cr);
  }

  /* --- Pass 2: directory header strip backgrounds --- */
  cairo_set_source_rgba(cr, 0.0, 0.0, 0.0, 0.58);
  for (i = 0; i < self->dir_label_count; i++) {
    const TreemapDirLabel *L = &self->dir_labels[i];
    cairo_rectangle(cr, L->x, L->y, L->w, L->h);
    cairo_fill(cr);
  }

  /* --- Pass 3: hover / selected border on file/dir rects --- */
  cairo_set_source_rgba(cr, HOVER_BORDER_R, HOVER_BORDER_G, HOVER_BORDER_B, HOVER_BORDER_A);
  for (i = 0; i < self->rect_count; i++) {
    const treemap_rect_t *R = &self->rects[i];
    gboolean sel = treemap_rect_is_selected(self, (gint)i);
    gboolean hov = ((gint)i == self->hovered_index);
    double bw, off;
    if (!sel && !hov) {
      continue;
    }
    bw  = sel ? SELECT_BORDER_W : HOVER_BORDER_W;
    off = bw / 2.0;
    cairo_set_line_width(cr, bw);
    cairo_rectangle(cr, R->x + off, R->y + off, R->w - bw, R->h - bw);
    cairo_stroke(cr);
  }

  /* --- Pass 3b: folder group outlines (selected = thick, hovered = thin) --- */
  cairo_set_source_rgba(cr, HOVER_BORDER_R, HOVER_BORDER_G, HOVER_BORDER_B, HOVER_BORDER_A);
  for (i = 0; i < self->dir_label_count; i++) {
    const TreemapDirLabel *HL = &self->dir_labels[i];
    gboolean sel = treemap_dir_is_selected(self, (gint)i);
    gboolean hov = ((gint)i == self->hovered_dir_index);
    double bw, off;
    if (!sel && !hov) {
      continue;
    }
    bw  = sel ? SELECT_BORDER_W : HOVER_BORDER_W;
    off = bw / 2.0;
    cairo_set_line_width(cr, bw);
    cairo_rectangle(cr, HL->x + off, HL->y + off, HL->w - bw, HL->dir_h - bw);
    cairo_stroke(cr);
  }

  /* --- Pass 4: file rect labels (dynamic font size based on rect area) --- */
  cairo_set_source_rgb(cr, 0.94, 0.94, 0.95);
  for (i = 0; i < self->rect_count; i++) {
    const treemap_rect_t *R = &self->rects[i];
    const char *txt;
    PangoLayout *pl;

    if (R->w < MIN_LABEL_W || R->h < MIN_LABEL_H) {
      continue;
    }
    txt = (self->rect_labels != NULL) ? self->rect_labels[i] : NULL;
    if (txt == NULL) {
      continue;
    }

    /* Use normal-font layout for large rects; small-font for medium rects. */
    if (R->w >= LARGE_W && R->h >= LARGE_H && self->pango_layout != NULL) {
      pl = self->pango_layout;
    } else if (self->pango_layout_sm != NULL) {
      pl = self->pango_layout_sm;
    } else {
      pl = self->pango_layout;
    }
    if (pl == NULL) {
      continue;
    }

    pango_layout_set_width(pl, (int)((R->w - 5.0) * PANGO_SCALE));
    pango_layout_set_ellipsize(pl, PANGO_ELLIPSIZE_END);
    pango_layout_set_text(pl, txt, -1);

    cairo_save(cr);
    cairo_rectangle(cr, R->x, R->y, R->w, R->h);
    cairo_clip(cr);
    cairo_move_to(cr, R->x + 3.0, R->y + 2.0);
    pango_cairo_show_layout(cr, pl);
    cairo_restore(cr);
  }

  /* --- Pass 5: directory header strip labels (font sized to DIR_HEADER_H) --- */
  if (self->pango_layout_hdr != NULL) {
    cairo_set_source_rgb(cr, 0.97, 0.97, 0.98);
    for (i = 0; i < self->dir_label_count; i++) {
      const TreemapDirLabel *L = &self->dir_labels[i];
      pango_layout_set_width(self->pango_layout_hdr, (int)((L->w - 6.0) * PANGO_SCALE));
      pango_layout_set_ellipsize(self->pango_layout_hdr, PANGO_ELLIPSIZE_END);
      pango_layout_set_text(self->pango_layout_hdr, L->text, -1);

      cairo_save(cr);
      cairo_rectangle(cr, L->x, L->y, L->w, L->h);
      cairo_clip(cr);
      cairo_move_to(cr, L->x + 3.0, L->y + 2.0);
      pango_cairo_show_layout(cr, self->pango_layout_hdr);
      cairo_restore(cr);
    }
  }

  return FALSE;
}

/* ---- input events -------------------------------------------------------- */

static gboolean treemap_motion(GtkWidget *w, GdkEventMotion *ev, gpointer user_data) {
  TreemapWidget *self = TREEMAP_WIDGET(user_data);
  int hit     = treemap_hit_index(self, ev->x, ev->y);
  int hit_dir = treemap_hit_dir_header(self, ev->x, ev->y);
  (void)w;
  if (hit != self->hovered_index || hit_dir != self->hovered_dir_index) {
    self->hovered_index     = hit;
    self->hovered_dir_index = hit_dir;
    gtk_widget_queue_draw(GTK_WIDGET(self));
    if (self->on_hover != NULL) {
      gint64 ix;
      if (hit_dir >= 0 && (size_t)hit_dir < self->dir_label_count &&
          self->dir_labels[hit_dir].scan_ix != (size_t)-1) {
        ix = (gint64)self->dir_labels[hit_dir].scan_ix;
      } else {
        ix = (hit < 0) ? -1 : (gint64)self->rects[hit].node_index;
      }
      self->on_hover(GTK_WIDGET(self), ix, self->on_hover_data);
    }
  }
  return FALSE;
}

static gboolean treemap_leave(GtkWidget *w, GdkEventCrossing *ev, gpointer user_data) {
  TreemapWidget *self = TREEMAP_WIDGET(user_data);
  (void)w;
  (void)ev;
  if (self->hovered_index >= 0 || self->hovered_dir_index >= 0) {
    self->hovered_index     = -1;
    self->hovered_dir_index = -1;
    gtk_widget_queue_draw(GTK_WIDGET(self));
    if (self->on_hover != NULL) {
      self->on_hover(GTK_WIDGET(self), -1, self->on_hover_data);
    }
  }
  return FALSE;
}

static gboolean treemap_button_press(GtkWidget *w, GdkEventButton *ev, gpointer user_data) {
  TreemapWidget *self = TREEMAP_WIDGET(user_data);
  int hit, hit_dir;
  gint64 ix;
  gboolean ctrl_held, shift_held;
  (void)w;
  if (ev->button != 1U) {
    return FALSE;
  }
  hit      = treemap_hit_index(self, ev->x, ev->y);
  hit_dir  = treemap_hit_dir_header(self, ev->x, ev->y);
  ctrl_held  = (ev->state & GDK_CONTROL_MASK) != 0;
  shift_held = (ev->state & GDK_SHIFT_MASK) != 0;

  if (hit_dir >= 0) {
    gint idx = (gint)hit_dir;
    if (ctrl_held || shift_held) {
      /* Ctrl/Shift: toggle dir in its array; don't touch rect selection. */
      treemap_dir_toggle(self, idx);
    } else {
      /* Plain click: clear both arrays, select this dir only. */
      g_array_set_size(self->selected_rect_indices, 0);
      g_array_set_size(self->selected_dir_indices, 0);
      self->anchor_rect_index = -1;
      g_array_append_val(self->selected_dir_indices, idx);
    }
    ix = (self->dir_labels[hit_dir].scan_ix != (size_t)-1)
         ? (gint64)self->dir_labels[hit_dir].scan_ix : -1;
  } else if (hit >= 0) {
    gint idx = (gint)hit;
    if (ctrl_held || shift_held) {
      /* Ctrl/Shift: toggle rect in its array; don't touch dir selection. */
      treemap_rect_toggle(self, idx);
    } else {
      /* Plain click: clear both arrays, select this rect only. */
      g_array_set_size(self->selected_rect_indices, 0);
      g_array_set_size(self->selected_dir_indices, 0);
      g_array_append_val(self->selected_rect_indices, idx);
      self->anchor_rect_index = idx;
    }
    ix = (gint64)self->rects[hit].node_index;
  } else {
    /* Click on empty space: clear all. */
    if (!ctrl_held && !shift_held) {
      g_array_set_size(self->selected_rect_indices, 0);
      g_array_set_size(self->selected_dir_indices, 0);
      self->anchor_rect_index = -1;
    }
    ix = -1;
  }

  treemap_persist_sync_from_visual(self);

  gtk_widget_queue_draw(GTK_WIDGET(self));
  if (self->on_selected != NULL) {
    self->on_selected(GTK_WIDGET(self), ix, self->on_selected_data);
  }
  return FALSE;
}

/* ---- size allocation ----------------------------------------------------- */

static void treemap_size_allocate(GtkWidget *widget, GdkRectangle *allocation, gpointer user_data) {
  TreemapWidget *self = TREEMAP_WIDGET(user_data);
  (void)widget;
  if (allocation->width != self->alloc_w || allocation->height != self->alloc_h || !self->layout_ok) {
    if (self->tree_root != NULL && allocation->width >= 2 && allocation->height >= 2) {
      treemap_run_layout(self, allocation->width, allocation->height);
    }
  }
}

/* ---- realize / unrealize ------------------------------------------------- */

static void treemap_realize(GtkWidget *widget, gpointer user_data) {
  TreemapWidget *self = TREEMAP_WIDGET(user_data);
  PangoFontDescription *fd;

  if (self->pango_layout == NULL) {
    self->pango_layout = gtk_widget_create_pango_layout(widget, "");
    pango_layout_set_single_paragraph_mode(self->pango_layout, TRUE);
    /* Explicit 9pt font for normal (large rect) labels. */
    fd = pango_font_description_copy(
        pango_context_get_font_description(pango_layout_get_context(self->pango_layout)));
    pango_font_description_set_size(fd, 9 * PANGO_SCALE);
    pango_layout_set_font_description(self->pango_layout, fd);
    pango_font_description_free(fd);
  }

  if (self->pango_layout_sm == NULL) {
    self->pango_layout_sm = gtk_widget_create_pango_layout(widget, "");
    pango_layout_set_single_paragraph_mode(self->pango_layout_sm, TRUE);
    /* 7pt font for small (medium rect) labels. */
    fd = pango_font_description_copy(
        pango_context_get_font_description(pango_layout_get_context(self->pango_layout_sm)));
    pango_font_description_set_size(fd, 7 * PANGO_SCALE);
    pango_layout_set_font_description(self->pango_layout_sm, fd);
    pango_font_description_free(fd);
  }

  if (self->pango_layout_hdr == NULL) {
    /*
     * Font size for the directory header strip, derived from DIR_HEADER_H so
     * it auto-scales whenever that constant changes:
     *   usable height = DIR_HEADER_H - 4 px  (2 px top + 2 px bottom padding)
     *   pts = px * 0.75                       (96 DPI: 1 px = 72/96 pt)
     */
    int hdr_pts = (int)((DIR_HEADER_H - 4.0) * 0.75);
    if (hdr_pts < 6) hdr_pts = 6;
    self->pango_layout_hdr = gtk_widget_create_pango_layout(widget, "");
    pango_layout_set_single_paragraph_mode(self->pango_layout_hdr, TRUE);
    fd = pango_font_description_copy(
        pango_context_get_font_description(pango_layout_get_context(self->pango_layout_hdr)));
    pango_font_description_set_size(fd, hdr_pts * PANGO_SCALE);

    pango_layout_set_font_description(self->pango_layout_hdr, fd);
    pango_font_description_free(fd);
  }
}

static void treemap_unrealize(GtkWidget *widget, gpointer user_data) {
  TreemapWidget *self = TREEMAP_WIDGET(user_data);
  (void)widget;
  g_clear_object(&self->pango_layout);
  g_clear_object(&self->pango_layout_sm);
  g_clear_object(&self->pango_layout_hdr);
}

/* ---- GObject lifecycle --------------------------------------------------- */

static void treemap_widget_finalize(GObject *object) {
  TreemapWidget *self = TREEMAP_WIDGET(object);
  g_free(self->root_utf8);
  treemap_clear_tree(self);
  treemap_clear_layout_buffers(self);
  if (self->selected_rect_indices) {
    g_array_free(self->selected_rect_indices, TRUE);
    self->selected_rect_indices = NULL;
  }
  if (self->selected_dir_indices) {
    g_array_free(self->selected_dir_indices, TRUE);
    self->selected_dir_indices = NULL;
  }
  if (self->persist_sel_nids != NULL) {
    g_array_free(self->persist_sel_nids, TRUE);
    self->persist_sel_nids = NULL;
  }
  g_clear_object(&self->pango_layout);
  g_clear_object(&self->pango_layout_sm);
  g_clear_object(&self->pango_layout_hdr);
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
  self->hovered_index        = -1;
  self->hovered_dir_index    = -1;
  self->selected_rect_indices = g_array_new(FALSE, FALSE, sizeof(gint));
  self->selected_dir_indices  = g_array_new(FALSE, FALSE, sizeof(gint));
  self->persist_sel_nids      = g_array_new(FALSE, FALSE, sizeof(size_t));
  self->anchor_rect_index    = -1;
  self->alloc_w = -1;
  self->alloc_h = -1;
  g_signal_connect(self, "size-allocate", G_CALLBACK(treemap_size_allocate), self);
  g_signal_connect(self, "realize", G_CALLBACK(treemap_realize), self);
  g_signal_connect(self, "unrealize", G_CALLBACK(treemap_unrealize), self);
  g_signal_connect(self, "motion-notify-event", G_CALLBACK(treemap_motion), self);
  g_signal_connect(self, "leave-notify-event", G_CALLBACK(treemap_leave), self);
  g_signal_connect(self, "button-press-event", G_CALLBACK(treemap_button_press), self);
}

/* ---- public API ---------------------------------------------------------- */

GtkWidget *treemap_widget_new(void) {
  return GTK_WIDGET(g_object_new(TREEMAP_TYPE_WIDGET, NULL));
}

void treemap_widget_set_data(TreemapWidget *widget, const char *root_utf8, const file_node_t *nodes,
                             size_t count) {
  GtkAllocation a;
  const file_node_t *prev_nodes;
  size_t prev_count;

  g_return_if_fail(TREEMAP_IS_WIDGET(widget));

  prev_nodes = widget->nodes;
  prev_count = widget->node_count;
  if (prev_nodes != nodes || prev_count != count) {
    if (widget->persist_sel_nids != NULL) {
      g_array_set_size(widget->persist_sel_nids, 0);
    }
  } else {
    treemap_persist_sync_from_visual(widget);
  }

  g_free(widget->root_utf8);
  widget->root_utf8 = g_strdup(root_utf8 != NULL ? root_utf8 : "");
  widget->nodes = nodes;
  widget->node_count = count;

  treemap_clear_layout_buffers(widget);
  treemap_clear_tree(widget);
  widget->layout_ok        = FALSE;
  widget->hovered_index    = -1;
  widget->hovered_dir_index = -1;
  if (widget->selected_rect_indices) g_array_set_size(widget->selected_rect_indices, 0);
  if (widget->selected_dir_indices)  g_array_set_size(widget->selected_dir_indices,  0);
  widget->anchor_rect_index = -1;

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

void treemap_widget_add_to_selection_by_scan_index(TreemapWidget *w, gint64 scan_index) {
  size_t i;
  g_return_if_fail(TREEMAP_IS_WIDGET(w));

  if (scan_index < 0 || !w->layout_ok) {
    return;
  }

  /* Search file rects first. */
  for (i = 0; i < w->rect_count; i++) {
    if ((gint64)w->rects[i].node_index == scan_index) {
      gint idx = (gint)i;
      if (!treemap_rect_is_selected(w, idx)) {
        g_array_append_val(w->selected_rect_indices, idx);
      }
      gtk_widget_queue_draw(GTK_WIDGET(w));
      return;
    }
  }

  /* Then search dir header labels. */
  for (i = 0; i < w->dir_label_count; i++) {
    if (w->dir_labels[i].scan_ix != (size_t)-1 &&
        (gint64)w->dir_labels[i].scan_ix == scan_index) {
      gint idx = (gint)i;
      if (!treemap_dir_is_selected(w, idx)) {
        g_array_append_val(w->selected_dir_indices, idx);
      }
      gtk_widget_queue_draw(GTK_WIDGET(w));
      return;
    }
  }
}

void treemap_widget_set_selection_by_scan_index(TreemapWidget *w, gint64 scan_index) {
  size_t i;
  g_return_if_fail(TREEMAP_IS_WIDGET(w));

  /* Clear existing selection. */
  if (w->selected_rect_indices) g_array_set_size(w->selected_rect_indices, 0);
  if (w->selected_dir_indices)  g_array_set_size(w->selected_dir_indices,  0);
  w->anchor_rect_index = -1;

  if (scan_index < 0 || !w->layout_ok) {
    gtk_widget_queue_draw(GTK_WIDGET(w));
    treemap_persist_sync_from_visual(w);
    return;
  }

  /* Search file rects first. */
  for (i = 0; i < w->rect_count; i++) {
    if ((gint64)w->rects[i].node_index == scan_index) {
      gint idx = (gint)i;
      g_array_append_val(w->selected_rect_indices, idx);
      w->anchor_rect_index = idx;
      gtk_widget_queue_draw(GTK_WIDGET(w));
      treemap_persist_sync_from_visual(w);
      return;
    }
  }

  /* Then search dir header labels. */
  for (i = 0; i < w->dir_label_count; i++) {
    if (w->dir_labels[i].scan_ix != (size_t)-1 &&
        (gint64)w->dir_labels[i].scan_ix == scan_index) {
      gint idx = (gint)i;
      g_array_append_val(w->selected_dir_indices, idx);
      gtk_widget_queue_draw(GTK_WIDGET(w));
      treemap_persist_sync_from_visual(w);
      return;
    }
  }

  /* No matching tile — redraw to reflect cleared selection. */
  gtk_widget_queue_draw(GTK_WIDGET(w));
  treemap_persist_sync_from_visual(w);
}

static void treemap_append_unique_nid(GArray *out, size_t nid) {
  guint i;
  for (i = 0; i < out->len; i++) {
    if (g_array_index(out, size_t, i) == nid) {
      return;
    }
  }
  g_array_append_val(out, nid);
}

void treemap_widget_append_selected_scan_indices(TreemapWidget *w, GArray *out_nids) {
  guint i;
  g_return_if_fail(TREEMAP_IS_WIDGET(w));
  g_return_if_fail(out_nids != NULL);
  if (w->layout_ok && w->nodes != NULL && w->rects != NULL) {
    treemap_collect_selected_scan_nids_from_rects(w, out_nids);
  }
  if (out_nids->len == 0 && w->persist_sel_nids != NULL && w->node_count > 0) {
    for (i = 0; i < w->persist_sel_nids->len; i++) {
      size_t nid = g_array_index(w->persist_sel_nids, size_t, i);
      if (nid < w->node_count) {
        treemap_append_unique_nid(out_nids, nid);
      }
    }
  }
}
