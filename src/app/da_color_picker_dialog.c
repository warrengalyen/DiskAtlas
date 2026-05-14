#include "da_color_picker_dialog.h"

#include <math.h>

#include <glib.h>
#include <gtk/gtk.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

/* Strip: gradient column (DA_CP_STRIP_GRADIENT_H px) + arrow column.
   Padding bands (ARROW_HALF px top/bottom) are theme background only — arrow tip
   still travels the full inner gradient range. */
#define DA_CP_STRIP_GRADIENT_W 28
#define DA_CP_STRIP_ARROW_W 12
#define DA_CP_STRIP_TOTAL_W (DA_CP_STRIP_GRADIENT_W + DA_CP_STRIP_ARROW_W)
#define DA_CP_STRIP_ARROW_HALF 7
#define DA_CP_STRIP_GRADIENT_H 220
#define DA_CP_STRIP_H          (DA_CP_STRIP_GRADIENT_H + 2 * DA_CP_STRIP_ARROW_HALF)

typedef enum {
  DA_CP_VAR_H = 0,
  DA_CP_VAR_S = 1,
  DA_CP_VAR_V = 2,
  DA_CP_VAR_R = 3,
  DA_CP_VAR_G = 4,
  DA_CP_VAR_B = 5,
} DaCpVar;

typedef struct DaColorPicker {
  GtkWidget *dialog;
  GtkWidget *plane;
  GtkWidget *strip;
  GtkWidget *swatch_new;
  GtkWidget *swatch_old;
  GtkWidget *rad_h;
  GtkWidget *rad_s;
  GtkWidget *rad_v;
  GtkWidget *rad_r;
  GtkWidget *rad_g;
  GtkWidget *rad_b;
  GtkSpinButton *spin_h;
  GtkSpinButton *spin_s;
  GtkSpinButton *spin_v;
  GtkSpinButton *spin_r;
  GtkSpinButton *spin_g;
  GtkSpinButton *spin_b;
  GtkEntry *hex_entry;
  GdkRGBA current;
  GdkRGBA original;
  gboolean frozen;
  gboolean plane_dirty;
  GdkPixbuf *plane_pb;
  gint plane_w;
  gint plane_h;
} DaColorPicker;

static gdouble da_cp_clamp01(gdouble x) {
  if (x < 0.0) {
    return 0.0;
  }
  if (x > 1.0) {
    return 1.0;
  }
  return x;
}

/* Strip coordinate helpers.
   Widget height = DA_CP_STRIP_H (gradient + 2×ARROW_HALF padding).
   The ARROW_HALF padding at each end gives the chevron body room at the extremes.
   Gradient content (t=0..1) maps to widget rows [ARROW_HALF, h-1-ARROW_HALF]. */
static gdouble da_cp_strip_yy_to_t(gint h, gint yy) {
  const gint g = DA_CP_STRIP_ARROW_HALF;
  const gint inner = h - 1 - 2 * g;
  if (inner <= 0) {
    return 0.5;
  }
  return da_cp_clamp01(((gdouble)yy - (gdouble)g) / (gdouble)inner);
}

static gdouble da_cp_strip_y_local_to_t(gint h, gdouble fy) {
  const gint g = DA_CP_STRIP_ARROW_HALF;
  const gint inner = h - 1 - 2 * g;
  fy = CLAMP(fy, 0.0, (gdouble)(h - 1));
  if (inner <= 0) {
    return 0.5;
  }
  return da_cp_clamp01((fy - (gdouble)g) / (gdouble)inner);
}

static gdouble da_cp_strip_t_to_tip_y(gint h, gdouble t) {
  const gint g = DA_CP_STRIP_ARROW_HALF;
  const gint inner = h - 1 - 2 * g;
  t = da_cp_clamp01(t);
  if (inner <= 0) {
    return (gdouble)(h / 2);
  }
  return (gdouble)g + t * (gdouble)inner;
}

static void da_cp_pick_strip(DaColorPicker *p, gdouble t);

static GdkSeat *da_cp_event_seat(GdkEvent *ev) {
  GdkSeat *seat = gdk_event_get_seat(ev);
  if (seat != NULL) {
    return seat;
  }
  GdkDevice *dev = gdk_event_get_device(ev);
  return (dev != NULL) ? gdk_device_get_seat(dev) : NULL;
}

static void da_cp_strip_pick_from_event(DaColorPicker *p, GtkWidget *w, GdkEvent *ev) {
  GtkAllocation a;
  gtk_widget_get_allocation(w, &a);
  gint h = a.height;
  /* Coordinates from the device grab are strip-window-relative; clamp at extremes. */
  gdouble raw_y = (ev->type == GDK_MOTION_NOTIFY) ? ev->motion.y : ev->button.y;
  gdouble t = da_cp_strip_y_local_to_t(h, raw_y);
  da_cp_pick_strip(p, t);
}

static void da_rgb_to_hsv(gdouble r, gdouble g, gdouble b, gdouble *h_out, gdouble *s_out, gdouble *v_out) {
  gdouble max = fmax(r, fmax(g, b));
  gdouble min = fmin(r, fmin(g, b));
  gdouble d = max - min;
  gdouble v = max;
  gdouble s = (max > 1e-9) ? d / max : 0.0;
  gdouble h = 0.0;
  if (d > 1e-9) {
    if (max == r) {
      h = 60.0 * fmod((g - b) / d, 6.0);
    } else if (max == g) {
      h = 60.0 * (((b - r) / d) + 2.0);
    } else {
      h = 60.0 * (((r - g) / d) + 4.0);
    }
    if (h < 0.0) {
      h += 360.0;
    }
  }
  *h_out = h;
  *s_out = s;
  *v_out = v;
}

static void da_hsv_to_rgb(gdouble h, gdouble s, gdouble v, gdouble *r_out, gdouble *g_out, gdouble *b_out) {
  s = da_cp_clamp01(s);
  v = da_cp_clamp01(v);
  while (h < 0.0) {
    h += 360.0;
  }
  while (h >= 360.0) {
    h -= 360.0;
  }
  gdouble c = v * s;
  gdouble x = c * (1.0 - fabs(fmod(h / 60.0, 2.0) - 1.0));
  gdouble m = v - c;
  gdouble rp = 0, gp = 0, bp = 0;
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
  *r_out = da_cp_clamp01(rp + m);
  *g_out = da_cp_clamp01(gp + m);
  *b_out = da_cp_clamp01(bp + m);
}

static void da_cp_rgba_to_hsv(const GdkRGBA *c, gdouble *h, gdouble *s, gdouble *v) {
  da_rgb_to_hsv(c->red, c->green, c->blue, h, s, v);
}

static DaCpVar da_cp_get_var(DaColorPicker *p) {
  if (gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(p->rad_h))) {
    return DA_CP_VAR_H;
  }
  if (gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(p->rad_s))) {
    return DA_CP_VAR_S;
  }
  if (gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(p->rad_v))) {
    return DA_CP_VAR_V;
  }
  if (gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(p->rad_r))) {
    return DA_CP_VAR_R;
  }
  if (gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(p->rad_g))) {
    return DA_CP_VAR_G;
  }
  return DA_CP_VAR_B;
}

static void da_cp_set_rgba(DaColorPicker *p, gdouble r, gdouble g, gdouble b) {
  p->current.red = da_cp_clamp01(r);
  p->current.green = da_cp_clamp01(g);
  p->current.blue = da_cp_clamp01(b);
  p->current.alpha = 1.0;
}

static void da_cp_plane_rgb_at(DaColorPicker *p, DaCpVar mode, gdouble fx, gdouble fy, gdouble *r, gdouble *g,
                               gdouble *b) {
  gdouble hh, ss, vv;
  da_cp_rgba_to_hsv(&p->current, &hh, &ss, &vv);
  gdouble rr = p->current.red;
  gdouble gg = p->current.green;
  gdouble bb = p->current.blue;
  fx = da_cp_clamp01(fx);
  fy = da_cp_clamp01(fy);
  switch (mode) {
  case DA_CP_VAR_H:
    da_hsv_to_rgb(hh, fx, 1.0 - fy, r, g, b);
    break;
  case DA_CP_VAR_S:
    da_hsv_to_rgb(fx * 360.0, ss, 1.0 - fy, r, g, b);
    break;
  case DA_CP_VAR_V:
    da_hsv_to_rgb(fx * 360.0, 1.0 - fy, vv, r, g, b);
    break;
  case DA_CP_VAR_R:
    *r = rr;
    *g = fx;
    *b = 1.0 - fy;
    break;
  case DA_CP_VAR_G:
    *r = fx;
    *g = gg;
    *b = 1.0 - fy;
    break;
  case DA_CP_VAR_B:
    *r = fx;
    *g = 1.0 - fy;
    *b = bb;
    break;
  }
}

static void da_cp_pick_plane(DaColorPicker *p, gdouble fx, gdouble fy) {
  DaCpVar mode = da_cp_get_var(p);
  gdouble r, g, b;
  da_cp_plane_rgb_at(p, mode, fx, fy, &r, &g, &b);
  da_cp_set_rgba(p, r, g, b);
}

static gdouble da_cp_strip_t_from_color(DaColorPicker *p, DaCpVar mode) {
  gdouble hh, ss, vv;
  da_cp_rgba_to_hsv(&p->current, &hh, &ss, &vv);
  switch (mode) {
  case DA_CP_VAR_H:
    return da_cp_clamp01(hh / 360.0);
  case DA_CP_VAR_S:
    return da_cp_clamp01(ss);
  case DA_CP_VAR_V:
    return da_cp_clamp01(vv);
  case DA_CP_VAR_R:
    return da_cp_clamp01(p->current.red);
  case DA_CP_VAR_G:
    return da_cp_clamp01(p->current.green);
  case DA_CP_VAR_B:
    return da_cp_clamp01(p->current.blue);
  }
  return 0;
}

static void da_cp_pick_strip(DaColorPicker *p, gdouble t) {
  t = da_cp_clamp01(t);
  DaCpVar mode = da_cp_get_var(p);
  gdouble hh, ss, vv;
  da_cp_rgba_to_hsv(&p->current, &hh, &ss, &vv);
  gdouble r = p->current.red, g = p->current.green, b = p->current.blue;
  switch (mode) {
  case DA_CP_VAR_H:
    /* Cap at 359.9999: t=1.0 would give H=360°, which da_hsv_to_rgb normalises
       to H=0° (same hue), making da_cp_strip_t_from_color return 0 → arrow jumps to top. */
    da_hsv_to_rgb(MIN(t * 360.0, 359.9999), ss, vv, &r, &g, &b);
    break;
  case DA_CP_VAR_S:
    da_hsv_to_rgb(hh, t, vv, &r, &g, &b);
    break;
  case DA_CP_VAR_V:
    da_hsv_to_rgb(hh, ss, t, &r, &g, &b);
    break;
  case DA_CP_VAR_R:
    r = t;
    break;
  case DA_CP_VAR_G:
    g = t;
    break;
  case DA_CP_VAR_B:
    b = t;
    break;
  }
  da_cp_set_rgba(p, r, g, b);
}

static void da_cp_strip_rgb_at(DaColorPicker *p, DaCpVar mode, gdouble t, gdouble *r, gdouble *g, gdouble *b) {
  gdouble hh, ss, vv;
  da_cp_rgba_to_hsv(&p->current, &hh, &ss, &vv);
  switch (mode) {
  case DA_CP_VAR_H:
    da_hsv_to_rgb(t * 360.0, ss, vv, r, g, b);
    break;
  case DA_CP_VAR_S:
    da_hsv_to_rgb(hh, t, vv, r, g, b);
    break;
  case DA_CP_VAR_V:
    da_hsv_to_rgb(hh, ss, t, r, g, b);
    break;
  case DA_CP_VAR_R:
    *r = t;
    *g = p->current.green;
    *b = p->current.blue;
    break;
  case DA_CP_VAR_G:
    *r = p->current.red;
    *g = t;
    *b = p->current.blue;
    break;
  case DA_CP_VAR_B:
    *r = p->current.red;
    *g = p->current.green;
    *b = t;
    break;
  }
}

static void da_cp_marker_plane(DaColorPicker *p, DaCpVar mode, gdouble *fx, gdouble *fy) {
  gdouble hh, ss, vv;
  da_cp_rgba_to_hsv(&p->current, &hh, &ss, &vv);
  gdouble rr = p->current.red, gg = p->current.green, bb = p->current.blue;
  (void)rr;
  (void)gg;
  (void)bb;
  switch (mode) {
  case DA_CP_VAR_H:
    *fx = da_cp_clamp01(ss);
    *fy = 1.0 - da_cp_clamp01(vv);
    break;
  case DA_CP_VAR_S:
    *fx = da_cp_clamp01(hh / 360.0);
    *fy = 1.0 - da_cp_clamp01(vv);
    break;
  case DA_CP_VAR_V:
    *fx = da_cp_clamp01(hh / 360.0);
    *fy = 1.0 - da_cp_clamp01(ss);
    break;
  case DA_CP_VAR_R:
    *fx = da_cp_clamp01(p->current.green);
    *fy = 1.0 - da_cp_clamp01(p->current.blue);
    break;
  case DA_CP_VAR_G:
    *fx = da_cp_clamp01(p->current.red);
    *fy = 1.0 - da_cp_clamp01(p->current.blue);
    break;
  case DA_CP_VAR_B:
    *fx = da_cp_clamp01(p->current.red);
    *fy = 1.0 - da_cp_clamp01(p->current.green);
    break;
  }
}

static void da_cp_rebuild_plane_pb(DaColorPicker *p) {
  gint w = p->plane_w;
  gint h = p->plane_h;
  if (w < 2 || h < 2) {
    return;
  }
  if (p->plane_pb != NULL) {
    g_object_unref(p->plane_pb);
    p->plane_pb = NULL;
  }
  p->plane_pb = gdk_pixbuf_new(GDK_COLORSPACE_RGB, FALSE, 8, w, h);
  if (p->plane_pb == NULL) {
    return;
  }
  DaCpVar mode = da_cp_get_var(p);
  gint rs = gdk_pixbuf_get_rowstride(p->plane_pb);
  guchar *px = gdk_pixbuf_get_pixels(p->plane_pb);
  for (gint yy = 0; yy < h; yy++) {
    gdouble fy = (gdouble)yy / (gdouble)(h - 1);
    guchar *row = px + yy * rs;
    for (gint xx = 0; xx < w; xx++) {
      gdouble fx = (gdouble)xx / (gdouble)(w - 1);
      gdouble r, g, b;
      da_cp_plane_rgb_at(p, mode, fx, fy, &r, &g, &b);
      row[xx * 3 + 0] = (guchar)(da_cp_clamp01(r) * 255.0 + 0.5);
      row[xx * 3 + 1] = (guchar)(da_cp_clamp01(g) * 255.0 + 0.5);
      row[xx * 3 + 2] = (guchar)(da_cp_clamp01(b) * 255.0 + 0.5);
    }
  }
  p->plane_dirty = FALSE;
}

static gboolean da_cp_on_plane_draw(GtkWidget *w, cairo_t *cr, gpointer user_data) {
  DaColorPicker *p = user_data;
  GtkAllocation a;
  gtk_widget_get_allocation(w, &a);
  if (a.width != p->plane_w || a.height != p->plane_h) {
    p->plane_w = a.width;
    p->plane_h = a.height;
    p->plane_dirty = TRUE;
  }
  if (p->plane_pb == NULL || p->plane_dirty) {
    da_cp_rebuild_plane_pb(p);
  }
  if (p->plane_pb != NULL) {
    gdk_cairo_set_source_pixbuf(cr, p->plane_pb, 0, 0);
    cairo_paint(cr);
  }
  gdouble fx, fy;
  da_cp_marker_plane(p, da_cp_get_var(p), &fx, &fy);
  gdouble mx = fx * (gdouble)(MAX(p->plane_w - 1, 1));
  gdouble my = fy * (gdouble)(MAX(p->plane_h - 1, 1));
  cairo_set_line_width(cr, 2.0);
  cairo_set_source_rgb(cr, 1.0, 1.0, 1.0);
  cairo_arc(cr, mx, my, 5.0, 0, 2.0 * M_PI);
  cairo_stroke(cr);
  cairo_set_source_rgb(cr, 0.1, 0.1, 0.1);
  cairo_arc(cr, mx, my, 6.0, 0, 2.0 * M_PI);
  cairo_stroke(cr);
  (void)w;
  return FALSE;
}

static gboolean da_cp_on_strip_draw(GtkWidget *w, cairo_t *cr, gpointer user_data) {
  DaColorPicker *p = user_data;
  GtkAllocation a;
  gtk_widget_get_allocation(w, &a);
  gint width = a.width;
  gint height = a.height;
  if (width < 1 || height < 1) {
    return FALSE;
  }
  GtkStyleContext *dsc = gtk_widget_get_style_context(p->dialog != NULL ? p->dialog : w);
  gtk_render_background(dsc, cr, 0, 0, (gdouble)width, (gdouble)height);

  const gint g = DA_CP_STRIP_ARROW_HALF;
  const gint y_end = height - 1 - g;
  const gdouble grad_w = (gdouble)MIN(DA_CP_STRIP_GRADIENT_W, width);
  DaCpVar mode = da_cp_get_var(p);
  /* Hue/value strip: only the inner band — padding rows stay background (not endpoints). */
  for (gint yy = g; yy <= y_end; yy++) {
    gdouble t = da_cp_strip_yy_to_t(height, yy);
    gdouble r, gcol, b;
    da_cp_strip_rgb_at(p, mode, t, &r, &gcol, &b);
    cairo_set_source_rgb(cr, r, gcol, b);
    cairo_rectangle(cr, 0, (gdouble)yy, grad_w, 1.0);
    cairo_fill(cr);
  }
  gdouble ty = da_cp_strip_t_from_color(p, mode);
  gdouble tip_y = da_cp_strip_t_to_tip_y(height, ty);
  const gdouble ah = (gdouble)DA_CP_STRIP_ARROW_HALF;
  const gdouble aw = MIN(10.0, (gdouble)width - grad_w - 1.0);
  if (aw > 2.0) {
    gdouble tip_x = grad_w;
    gdouble mid_x = tip_x + aw;
    cairo_move_to(cr, tip_x, tip_y);
    cairo_line_to(cr, mid_x, tip_y - ah);
    cairo_line_to(cr, mid_x, tip_y + ah);
    cairo_close_path(cr);
    cairo_set_source_rgb(cr, 0.95, 0.95, 0.95);
    cairo_fill_preserve(cr);
    cairo_set_source_rgb(cr, 0.05, 0.05, 0.05);
    cairo_set_line_width(cr, 1.0);
    cairo_stroke(cr);
  }
  return FALSE;
}

static gboolean da_cp_on_swatch_draw(GtkWidget *w, cairo_t *cr, gpointer user_data) {
  GdkRGBA *c = user_data;
  GtkAllocation a;
  gtk_widget_get_allocation(w, &a);
  gdk_cairo_set_source_rgba(cr, c);
  cairo_rectangle(cr, 0, 0, (gdouble)a.width, (gdouble)a.height);
  cairo_fill(cr);
  cairo_set_source_rgb(cr, 0.35, 0.35, 0.35);
  cairo_set_line_width(cr, 1.0);
  cairo_rectangle(cr, 0.5, 0.5, (gdouble)a.width - 1.0, (gdouble)a.height - 1.0);
  cairo_stroke(cr);
  return FALSE;
}

static gchar *da_cp_hex_string(const GdkRGBA *c) {
  return g_strdup_printf("#%02X%02X%02X", (guint)(da_cp_clamp01(c->red) * 255.0 + 0.5),
                         (guint)(da_cp_clamp01(c->green) * 255.0 + 0.5),
                         (guint)(da_cp_clamp01(c->blue) * 255.0 + 0.5));
}

static void da_cp_refresh_widgets(DaColorPicker *p) {
  gdouble hh, ss, vv;
  da_cp_rgba_to_hsv(&p->current, &hh, &ss, &vv);
  gchar *hx = da_cp_hex_string(&p->current);

  p->frozen = TRUE;
  gtk_spin_button_set_value(p->spin_h, hh);
  gtk_spin_button_set_value(p->spin_s, ss * 100.0);
  gtk_spin_button_set_value(p->spin_v, vv * 100.0);
  gtk_spin_button_set_value(p->spin_r, p->current.red * 255.0);
  gtk_spin_button_set_value(p->spin_g, p->current.green * 255.0);
  gtk_spin_button_set_value(p->spin_b, p->current.blue * 255.0);
  gtk_entry_set_text(p->hex_entry, hx);
  p->frozen = FALSE;

  g_free(hx);
  p->plane_dirty = TRUE;
  gtk_widget_queue_draw(p->plane);
  gtk_widget_queue_draw(p->strip);
  gtk_widget_queue_draw(p->swatch_new);
}

static void da_cp_event_xy_norm(GtkWidget *w, GdkEvent *ev, gdouble *nx, gdouble *ny) {
  gdouble x;
  gdouble y;
  if (ev->type == GDK_BUTTON_PRESS || ev->type == GDK_BUTTON_RELEASE) {
    x = ev->button.x;
    y = ev->button.y;
  } else if (ev->type == GDK_MOTION_NOTIFY) {
    x = ev->motion.x;
    y = ev->motion.y;
  } else {
    x = y = 0.0;
  }
  GtkAllocation a;
  gtk_widget_get_allocation(w, &a);
  gint ix = (gint)floor(x + 0.5);
  gint iy = (gint)floor(y + 0.5);
  ix = CLAMP(ix, 0, MAX(a.width - 1, 0));
  iy = CLAMP(iy, 0, MAX(a.height - 1, 0));
  if (a.width > 1) {
    *nx = (gdouble)ix / (gdouble)(a.width - 1);
  } else {
    *nx = 0.5;
  }
  if (a.height > 1) {
    *ny = (gdouble)iy / (gdouble)(a.height - 1);
  } else {
    *ny = 0.5;
  }
  *nx = da_cp_clamp01(*nx);
  *ny = da_cp_clamp01(*ny);
}

static gboolean da_cp_on_plane_button(GtkWidget *w, GdkEvent *ev, gpointer user_data) {
  DaColorPicker *p = user_data;
  if (ev->type == GDK_MOTION_NOTIFY) {
    if ((ev->motion.state & GDK_BUTTON1_MASK) == 0) {
      return FALSE;
    }
  } else if (ev->type == GDK_BUTTON_PRESS) {
    if (ev->button.button != 1) {
      return FALSE;
    }
  } else {
    return FALSE;
  }
  gdouble nx, ny;
  da_cp_event_xy_norm(w, ev, &nx, &ny);
  da_cp_pick_plane(p, nx, ny);
  da_cp_refresh_widgets(p);
  return FALSE;
}

static gboolean da_cp_on_strip_button_release(GtkWidget *w, GdkEvent *ev, gpointer user_data) {
  (void)w;
  (void)user_data;
  if (ev->type == GDK_BUTTON_RELEASE && ev->button.button == 1) {
    GdkSeat *seat = da_cp_event_seat(ev);
    if (seat != NULL) {
      gdk_seat_ungrab(seat);
    }
  }
  return FALSE;
}

static gboolean da_cp_on_strip_grab_broken(GtkWidget *w, GdkEvent *ev, gpointer user_data) {
  (void)w;
  (void)ev;
  (void)user_data;
  return FALSE;
}

static gboolean da_cp_on_strip_button(GtkWidget *w, GdkEvent *ev, gpointer user_data) {
  DaColorPicker *p = user_data;
  if (ev->type == GDK_MOTION_NOTIFY) {
    if ((ev->motion.state & GDK_BUTTON1_MASK) == 0) {
      return FALSE;
    }
  } else if (ev->type == GDK_BUTTON_PRESS) {
    if (ev->button.button != 1) {
      return FALSE;
    }
    /* Hard OS-level grab: on Win32 this calls SetCapture, on X11 XGrabPointer.
       Motion events during the drag arrive with coordinates relative to the
       strip's own window (possibly negative or > height), which we clamp. */
    GdkSeat *seat = da_cp_event_seat(ev);
    GdkWindow *win = gtk_widget_get_window(w);
    if (seat != NULL && win != NULL) {
      gdk_seat_grab(seat, win, GDK_SEAT_CAPABILITY_POINTER, FALSE, NULL, ev, NULL, NULL);
    }
  } else {
    return FALSE;
  }
  da_cp_strip_pick_from_event(p, w, ev);
  da_cp_refresh_widgets(p);
  return FALSE;
}

static void da_cp_on_spin_hsv_changed(GtkSpinButton *spin, gpointer user_data) {
  (void)spin;
  DaColorPicker *p = user_data;
  if (p->frozen) {
    return;
  }
  gdouble hh = gtk_spin_button_get_value(p->spin_h);
  gdouble ss = gtk_spin_button_get_value(p->spin_s) / 100.0;
  gdouble vv = gtk_spin_button_get_value(p->spin_v) / 100.0;
  gdouble r, g, b;
  da_hsv_to_rgb(hh, ss, vv, &r, &g, &b);
  da_cp_set_rgba(p, r, g, b);
  da_cp_refresh_widgets(p);
}

static void da_cp_on_spin_rgb_changed(GtkSpinButton *spin, gpointer user_data) {
  (void)spin;
  DaColorPicker *p = user_data;
  if (p->frozen) {
    return;
  }
  gdouble r = gtk_spin_button_get_value(p->spin_r) / 255.0;
  gdouble g = gtk_spin_button_get_value(p->spin_g) / 255.0;
  gdouble b = gtk_spin_button_get_value(p->spin_b) / 255.0;
  da_cp_set_rgba(p, r, g, b);
  da_cp_refresh_widgets(p);
}

static void da_cp_on_hex_changed(GtkEditable *ed, gpointer user_data) {
  (void)ed;
  DaColorPicker *p = user_data;
  if (p->frozen) {
    return;
  }
  const gchar *t = gtk_entry_get_text(p->hex_entry);
  GdkRGBA c;
  if (t != NULL && gdk_rgba_parse(&c, t)) {
    da_cp_set_rgba(p, c.red, c.green, c.blue);
    da_cp_refresh_widgets(p);
  }
}

static void da_cp_on_mode_toggled(GtkToggleButton *tb, gpointer user_data) {
  (void)tb;
  DaColorPicker *p = user_data;
  if (p->frozen) {
    return;
  }
  p->plane_dirty = TRUE;
  gtk_widget_queue_draw(p->plane);
  gtk_widget_queue_draw(p->strip);
}

static void da_cp_attach_plane_events(GtkWidget *w, gpointer user_data) {
  gtk_widget_add_events(w, GDK_BUTTON1_MOTION_MASK | GDK_BUTTON_PRESS_MASK);
  g_signal_connect(w, "button-press-event", G_CALLBACK(da_cp_on_plane_button), user_data);
  g_signal_connect(w, "motion-notify-event", G_CALLBACK(da_cp_on_plane_button), user_data);
}

static void da_cp_attach_strip_events(GtkWidget *w, gpointer user_data) {
  gtk_widget_add_events(w, GDK_BUTTON_PRESS_MASK | GDK_BUTTON_RELEASE_MASK | GDK_BUTTON1_MOTION_MASK);
  g_signal_connect(w, "button-press-event", G_CALLBACK(da_cp_on_strip_button), user_data);
  g_signal_connect(w, "motion-notify-event", G_CALLBACK(da_cp_on_strip_button), user_data);
  g_signal_connect(w, "button-release-event", G_CALLBACK(da_cp_on_strip_button_release), user_data);
  g_signal_connect(w, "grab-broken-event", G_CALLBACK(da_cp_on_strip_grab_broken), user_data);
}

static void da_cp_free(gpointer data) {
  DaColorPicker *p = data;
  if (p->plane_pb != NULL) {
    g_object_unref(p->plane_pb);
  }
  g_free(p);
}

static GtkWidget *da_cp_labeled_row(const gchar *radio_label, GtkWidget *rad, GtkWidget *spin, const gchar *suffix) {
  GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
  gtk_box_pack_start(GTK_BOX(row), rad, FALSE, FALSE, 0);
  GtkWidget *lb = gtk_label_new(radio_label);
  gtk_widget_set_halign(lb, GTK_ALIGN_START);
  gtk_box_pack_start(GTK_BOX(row), lb, FALSE, FALSE, 0);
  gtk_box_pack_start(GTK_BOX(row), GTK_WIDGET(spin), FALSE, FALSE, 0);
  if (suffix != NULL) {
    GtkWidget *suf = gtk_label_new(suffix);
    gtk_box_pack_start(GTK_BOX(row), suf, FALSE, FALSE, 0);
  }
  return row;
}

static GtkWidget *da_color_picker_build_ui(DaColorPicker *p) {
  GtkWidget *dlg = gtk_dialog_new_with_buttons("Choose category color", NULL,
                                               (GtkDialogFlags)(GTK_DIALOG_MODAL | GTK_DIALOG_DESTROY_WITH_PARENT),
                                               "_Cancel", GTK_RESPONSE_CANCEL, "_OK", GTK_RESPONSE_OK, NULL);
  p->dialog = dlg;
  gtk_window_set_default_size(GTK_WINDOW(dlg), 480, 360);
  gtk_window_set_resizable(GTK_WINDOW(dlg), FALSE);
  gtk_window_set_icon(GTK_WINDOW(dlg), NULL);

  GtkWidget *content = gtk_dialog_get_content_area(GTK_DIALOG(dlg));
  gtk_container_set_border_width(GTK_CONTAINER(content), 10);

  GtkWidget *dlg_shell = gtk_bin_get_child(GTK_BIN(dlg));
  if (dlg_shell != NULL && GTK_IS_BOX(dlg_shell)) {
    gtk_box_set_spacing(GTK_BOX(dlg_shell), 10);
  } else {
    gtk_widget_set_margin_bottom(content, 10);
  }

  GtkWidget *root = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
  gtk_box_pack_start(GTK_BOX(content), root, TRUE, TRUE, 0);

  GtkWidget *left = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
  gtk_widget_set_valign(left, GTK_ALIGN_START);
  gtk_box_pack_start(GTK_BOX(root), left, FALSE, FALSE, 0);

  GtkWidget *plane_aspect = gtk_aspect_frame_new(NULL, 0.5f, 0.5f, 1.0f, FALSE);
  gtk_aspect_frame_set(GTK_ASPECT_FRAME(plane_aspect), 0.5f, 0.5f, 1.0f, FALSE);
  gtk_widget_set_size_request(plane_aspect, 220, 220);
  /* Match 220px-tall gradient band to plane (strip is taller due to top/bottom padding). */
  gtk_widget_set_valign(plane_aspect, GTK_ALIGN_CENTER);

  p->plane = gtk_drawing_area_new();
  gtk_widget_set_hexpand(p->plane, FALSE);
  gtk_widget_set_vexpand(p->plane, FALSE);
  g_signal_connect(p->plane, "draw", G_CALLBACK(da_cp_on_plane_draw), p);
  da_cp_attach_plane_events(p->plane, p);
  gtk_container_add(GTK_CONTAINER(plane_aspect), p->plane);
  gtk_box_pack_start(GTK_BOX(left), plane_aspect, FALSE, FALSE, 0);

  p->strip = gtk_drawing_area_new();
  gtk_widget_set_valign(p->strip, GTK_ALIGN_START);
  gtk_widget_set_size_request(p->strip, DA_CP_STRIP_TOTAL_W, DA_CP_STRIP_H);
  g_signal_connect(p->strip, "draw", G_CALLBACK(da_cp_on_strip_draw), p);
  da_cp_attach_strip_events(p->strip, p);
  gtk_box_pack_start(GTK_BOX(left), p->strip, FALSE, FALSE, 0);

  GtkWidget *right = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);
  gtk_widget_set_valign(right, GTK_ALIGN_START);
  gtk_box_pack_start(GTK_BOX(root), right, TRUE, TRUE, 0);

  GtkWidget *cmp = gtk_grid_new();
  gtk_grid_set_row_spacing(GTK_GRID(cmp), 4);
  gtk_grid_set_column_spacing(GTK_GRID(cmp), 6);

  GtkWidget *ln = gtk_label_new("New");
  gtk_widget_set_halign(ln, GTK_ALIGN_START);
  gtk_grid_attach(GTK_GRID(cmp), ln, 0, 0, 1, 1);
  p->swatch_new = gtk_drawing_area_new();
  gtk_widget_set_size_request(p->swatch_new, 72, 28);
  g_signal_connect(p->swatch_new, "draw", G_CALLBACK(da_cp_on_swatch_draw), &p->current);
  gtk_grid_attach(GTK_GRID(cmp), p->swatch_new, 1, 0, 1, 1);

  GtkWidget *lc = gtk_label_new("Current");
  gtk_widget_set_halign(lc, GTK_ALIGN_START);
  gtk_grid_attach(GTK_GRID(cmp), lc, 0, 1, 1, 1);
  p->swatch_old = gtk_drawing_area_new();
  gtk_widget_set_size_request(p->swatch_old, 72, 28);
  g_signal_connect(p->swatch_old, "draw", G_CALLBACK(da_cp_on_swatch_draw), &p->original);
  gtk_grid_attach(GTK_GRID(cmp), p->swatch_old, 1, 1, 1, 1);

  gtk_box_pack_start(GTK_BOX(right), cmp, FALSE, FALSE, 0);

  GtkWidget *hsb_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
  gtk_box_pack_start(GTK_BOX(right), hsb_box, FALSE, FALSE, 0);

  p->rad_h = gtk_radio_button_new_with_label(NULL, "");
  p->rad_s = gtk_radio_button_new_with_label_from_widget(GTK_RADIO_BUTTON(p->rad_h), "");
  p->rad_v = gtk_radio_button_new_with_label_from_widget(GTK_RADIO_BUTTON(p->rad_h), "");

  p->spin_h = GTK_SPIN_BUTTON(gtk_spin_button_new_with_range(0, 359, 1));
  p->spin_s = GTK_SPIN_BUTTON(gtk_spin_button_new_with_range(0, 100, 1));
  p->spin_v = GTK_SPIN_BUTTON(gtk_spin_button_new_with_range(0, 100, 1));

  gtk_box_pack_start(GTK_BOX(hsb_box), da_cp_labeled_row("H:", p->rad_h, GTK_WIDGET(p->spin_h), "\302\260"), FALSE,
                     FALSE, 0);
  gtk_box_pack_start(GTK_BOX(hsb_box), da_cp_labeled_row("S:", p->rad_s, GTK_WIDGET(p->spin_s), "%"), FALSE, FALSE,
                     0);
  gtk_box_pack_start(GTK_BOX(hsb_box), da_cp_labeled_row("B:", p->rad_v, GTK_WIDGET(p->spin_v), "%"), FALSE, FALSE,
                     0);

  GtkWidget *rgb_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
  gtk_box_pack_start(GTK_BOX(right), rgb_box, FALSE, FALSE, 0);

  p->rad_r = gtk_radio_button_new_with_label_from_widget(GTK_RADIO_BUTTON(p->rad_h), "");
  p->rad_g = gtk_radio_button_new_with_label_from_widget(GTK_RADIO_BUTTON(p->rad_h), "");
  p->rad_b = gtk_radio_button_new_with_label_from_widget(GTK_RADIO_BUTTON(p->rad_h), "");

  p->spin_r = GTK_SPIN_BUTTON(gtk_spin_button_new_with_range(0, 255, 1));
  p->spin_g = GTK_SPIN_BUTTON(gtk_spin_button_new_with_range(0, 255, 1));
  p->spin_b = GTK_SPIN_BUTTON(gtk_spin_button_new_with_range(0, 255, 1));

  gtk_box_pack_start(GTK_BOX(rgb_box), da_cp_labeled_row("R:", p->rad_r, GTK_WIDGET(p->spin_r), NULL), FALSE, FALSE,
                     0);
  gtk_box_pack_start(GTK_BOX(rgb_box), da_cp_labeled_row("G:", p->rad_g, GTK_WIDGET(p->spin_g), NULL), FALSE, FALSE,
                     0);
  gtk_box_pack_start(GTK_BOX(rgb_box), da_cp_labeled_row("B:", p->rad_b, GTK_WIDGET(p->spin_b), NULL), FALSE, FALSE,
                     0);

  GtkWidget *hex_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
  GtkWidget *hxl = gtk_label_new("#");
  p->hex_entry = GTK_ENTRY(gtk_entry_new());
  gtk_entry_set_width_chars(p->hex_entry, 10);
  gtk_box_pack_start(GTK_BOX(hex_row), hxl, FALSE, FALSE, 0);
  gtk_box_pack_start(GTK_BOX(hex_row), GTK_WIDGET(p->hex_entry), TRUE, TRUE, 0);
  gtk_box_pack_start(GTK_BOX(right), hex_row, FALSE, FALSE, 0);

  g_signal_connect(p->spin_h, "value-changed", G_CALLBACK(da_cp_on_spin_hsv_changed), p);
  g_signal_connect(p->spin_s, "value-changed", G_CALLBACK(da_cp_on_spin_hsv_changed), p);
  g_signal_connect(p->spin_v, "value-changed", G_CALLBACK(da_cp_on_spin_hsv_changed), p);
  g_signal_connect(p->spin_r, "value-changed", G_CALLBACK(da_cp_on_spin_rgb_changed), p);
  g_signal_connect(p->spin_g, "value-changed", G_CALLBACK(da_cp_on_spin_rgb_changed), p);
  g_signal_connect(p->spin_b, "value-changed", G_CALLBACK(da_cp_on_spin_rgb_changed), p);
  g_signal_connect(p->hex_entry, "changed", G_CALLBACK(da_cp_on_hex_changed), p);

  g_signal_connect(p->rad_h, "toggled", G_CALLBACK(da_cp_on_mode_toggled), p);
  g_signal_connect(p->rad_s, "toggled", G_CALLBACK(da_cp_on_mode_toggled), p);
  g_signal_connect(p->rad_v, "toggled", G_CALLBACK(da_cp_on_mode_toggled), p);
  g_signal_connect(p->rad_r, "toggled", G_CALLBACK(da_cp_on_mode_toggled), p);
  g_signal_connect(p->rad_g, "toggled", G_CALLBACK(da_cp_on_mode_toggled), p);
  g_signal_connect(p->rad_b, "toggled", G_CALLBACK(da_cp_on_mode_toggled), p);

  g_object_set_data_full(G_OBJECT(dlg), "da-cp", p, da_cp_free);

  gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(p->rad_h), TRUE);

  return dlg;
}

gboolean da_color_picker_dialog_run(GtkWindow *parent, const GdkRGBA *initial,
                                    const GdkRGBA *original_for_compare, GdkRGBA *out) {
  if (initial == NULL || out == NULL) {
    return FALSE;
  }

  DaColorPicker *p = g_new0(DaColorPicker, 1);
  p->current = *initial;
  p->current.alpha = 1.0;
  if (original_for_compare != NULL) {
    p->original = *original_for_compare;
  } else {
    p->original = *initial;
  }
  p->original.alpha = 1.0;
  p->plane_dirty = TRUE;
  p->plane_w = 0;
  p->plane_h = 0;

  GtkWidget *dlg = da_color_picker_build_ui(p);
  gtk_window_set_transient_for(GTK_WINDOW(dlg), parent);
  gtk_widget_show_all(dlg);

  da_cp_refresh_widgets(p);

  gint resp = gtk_dialog_run(GTK_DIALOG(dlg));
  gboolean ok = (resp == GTK_RESPONSE_OK);
  if (ok) {
    *out = p->current;
    out->alpha = 1.0;
  }
  gtk_widget_destroy(dlg);
  return ok;
}
