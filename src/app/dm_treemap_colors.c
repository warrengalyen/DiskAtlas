#include <math.h>
#include <stddef.h>
#include <string.h>

#include <cairo.h>

#include "dm_treemap_colors.h"

static void dm_unpack_rgba_u32(uint32_t rgba, double *r, double *g, double *b, double *a) {
  *r = ((rgba >> 24) & 0xFFu) / 255.0;
  *g = ((rgba >> 16) & 0xFFu) / 255.0;
  *b = ((rgba >> 8) & 0xFFu) / 255.0;
  *a = (rgba & 0xFFu) / 255.0;
}

static uint32_t dm_pack_rgba_u8(uint8_t rr, uint8_t gg, uint8_t bb, uint8_t aa) {
  return ((uint32_t)rr << 24) | ((uint32_t)gg << 16) | ((uint32_t)bb << 8) | (uint32_t)aa;
}

void dm_file_node_compute_gradient_colors(file_node_t *node, double gradient_strength,
                                          double shadow_strength) {
  if (node == NULL) {
    return;
  }
  uint32_t base = node->mime_color_rgba;
  if (base == 0u) {
    base = DISKATLAS_MIME_COLOR_FALLBACK;
  }

  uint8_t r = (uint8_t)((base >> 24) & 0xFFu);
  uint8_t g = (uint8_t)((base >> 16) & 0xFFu);
  uint8_t b = (uint8_t)((base >> 8) & 0xFFu);
  uint8_t a = (uint8_t)(base & 0xFFu);

  double fh = gradient_strength < 0.0 ? 0.0 : (gradient_strength > 1.0 ? 1.0 : gradient_strength);
  double fs = shadow_strength < 0.0 ? 0.0 : (shadow_strength > 1.0 ? 1.0 : shadow_strength);

  double lr = r + (255.0 - (double)r) * fh;
  double lg = g + (255.0 - (double)g) * fh;
  double lb = b + (255.0 - (double)b) * fh;

  double sr = r + (0.0 - (double)r) * fs;
  double sg = g + (0.0 - (double)g) * fs;
  double sb = b + (0.0 - (double)b) * fs;

  node->gradient_light_rgba =
      dm_pack_rgba_u8((uint8_t)(lr + 0.5), (uint8_t)(lg + 0.5), (uint8_t)(lb + 0.5), a);
  node->gradient_dark_rgba =
      dm_pack_rgba_u8((uint8_t)(sr + 0.5), (uint8_t)(sg + 0.5), (uint8_t)(sb + 0.5), a);
}

void dm_file_nodes_refresh_gradient_colors(file_node_t *nodes, size_t count, double gradient_strength,
                                           double shadow_strength) {
  if (nodes == NULL || count == 0u) {
    return;
  }
  for (size_t i = 0; i < count; i++) {
    file_node_t *fn = &nodes[i];
    uint32_t kind = fn->attributes & DISKATLAS_NODE_KIND_MASK;
    if (kind == DISKATLAS_NODE_KIND_DIR) {
      fn->gradient_light_rgba = 0u;
      fn->gradient_dark_rgba = 0u;
      continue;
    }
    dm_file_node_compute_gradient_colors(fn, gradient_strength, shadow_strength);
  }
}

gboolean dm_treemap_tile_use_radial_gradient(double width, double height, const DmTreemapStyle *style) {
  if (style == NULL || !style->enable_tile_gradients) {
    return FALSE;
  }
  int mn = style->gradient_min_tile_size;
  if (mn < 0) {
    mn = 0;
  }
  if (width < (double)mn || height < (double)mn) {
    return FALSE;
  }
  if (width * height < DM_TREEMAP_AREA_SOLID_THRESHOLD) {
    return FALSE;
  }
  return TRUE;
}

gboolean dm_treemap_tile_show_border(double width, double height, const DmTreemapStyle *style) {
  if (style == NULL || !style->enable_tile_borders) {
    return FALSE;
  }
  int mn = style->gradient_min_tile_size;
  if (mn < 0) {
    mn = 0;
  }
  if (width < (double)mn || height < (double)mn) {
    return FALSE;
  }
  if (width * height < DM_TREEMAP_AREA_SOLID_THRESHOLD) {
    return FALSE;
  }
  return TRUE;
}

/**
 * Draws a 1-pixel raised bevel around a tile.
 * The top and left edges are painted with @p light_rgba (highlight),
 * the bottom and right edges with @p dark_rgba (shadow), giving the tile
 * a raised, WizTree-style appearance.
 */
static void dm_treemap_stroke_bevel(cairo_t *cr, double x, double y, double width, double height,
                                    uint32_t light_rgba, uint32_t dark_rgba,
                                    const DmTreemapStyle *style) {
  if (!dm_treemap_tile_show_border(width, height, style)) {
    return;
  }
  double hl_r, hl_g, hl_b, hl_a;
  double dk_r, dk_g, dk_b, dk_a;
  dm_unpack_rgba_u32(light_rgba, &hl_r, &hl_g, &hl_b, &hl_a);
  dm_unpack_rgba_u32(dark_rgba,  &dk_r, &dk_g, &dk_b, &dk_a);

  double ha = hl_a * style->border_strength;
  double sa = dk_a * style->border_strength;
  if (ha > 1.0) ha = 1.0;
  if (sa > 1.0) sa = 1.0;

  cairo_save(cr);
  cairo_set_line_width(cr, 1.0);

  /* Highlight: top edge then left edge. */
  cairo_set_source_rgba(cr, hl_r, hl_g, hl_b, ha);
  cairo_move_to(cr, x + 0.5,           y + height - 0.5);
  cairo_line_to(cr, x + 0.5,           y + 0.5);
  cairo_line_to(cr, x + width  - 0.5,  y + 0.5);
  cairo_stroke(cr);

  /* Shadow: right edge then bottom edge. */
  cairo_set_source_rgba(cr, dk_r, dk_g, dk_b, sa);
  cairo_move_to(cr, x + width  - 0.5,  y + 0.5);
  cairo_line_to(cr, x + width  - 0.5,  y + height - 0.5);
  cairo_line_to(cr, x + 0.5,           y + height - 0.5);
  cairo_stroke(cr);

  cairo_restore(cr);
}

void dm_treemap_draw_gradient_tile(cairo_t *cr, const file_node_t *node, double x, double y, double width,
                                   double height, const DmTreemapStyle *style) {
  if (cr == NULL || node == NULL || style == NULL || width < 1.0 || height < 1.0) {
    return;
  }

  uint32_t base = node->mime_color_rgba != 0u ? node->mime_color_rgba : DISKATLAS_MIME_COLOR_FALLBACK;
  double rf, gf, bf, af;
  dm_unpack_rgba_u32(base, &rf, &gf, &bf, &af);

  uint32_t light_rgba = node->gradient_light_rgba != 0u ? node->gradient_light_rgba : base;
  uint32_t dark_rgba = node->gradient_dark_rgba != 0u ? node->gradient_dark_rgba : base;

  double hl_r, hl_g, hl_b, hl_a;
  double dk_r, dk_g, dk_b, dk_a;
  dm_unpack_rgba_u32(light_rgba, &hl_r, &hl_g, &hl_b, &hl_a);
  dm_unpack_rgba_u32(dark_rgba, &dk_r, &dk_g, &dk_b, &dk_a);

  if (!dm_treemap_tile_use_radial_gradient(width, height, style)) {
    cairo_set_source_rgba(cr, rf, gf, bf, af);
    cairo_rectangle(cr, x, y, width, height);
    cairo_fill(cr);
    dm_treemap_stroke_bevel(cr, x, y, width, height, light_rgba, dark_rgba, style);
    return;
  }

  double cx = x + width * 0.30;
  double cy = y + height * 0.25;
  double radius = fmax(width, height) * 1.2;
  if (radius > 180.0) {
    radius = 180.0;
  }
  if (radius < 1.0) {
    radius = 1.0;
  }

  cairo_pattern_t *pat = cairo_pattern_create_radial(cx, cy, 0.0, cx, cy, radius);
  cairo_pattern_add_color_stop_rgba(pat, 0.0, hl_r, hl_g, hl_b, hl_a);
  cairo_pattern_add_color_stop_rgba(pat, 0.6, rf, gf, bf, af);
  cairo_pattern_add_color_stop_rgba(pat, 1.0, dk_r, dk_g, dk_b, dk_a);

  cairo_save(cr);
  cairo_rectangle(cr, x, y, width, height);
  cairo_clip(cr);
  cairo_new_path(cr);
  cairo_rectangle(cr, x, y, width, height);
  cairo_set_source(cr, pat);
  cairo_fill(cr);
  cairo_restore(cr);
  cairo_pattern_destroy(pat);

  dm_treemap_stroke_bevel(cr, x, y, width, height, light_rgba, dark_rgba, style);
}

void dm_treemap_draw_dir_gradient_tile(cairo_t *cr, double x, double y, double width, double height,
                                       const DmTreemapStyle *style) {
  if (cr == NULL || style == NULL || width < 1.0 || height < 1.0) {
    return;
  }

  file_node_t syn;
  memset(&syn, 0, sizeof(syn));
  syn.struct_version = DISKATLAS_FILE_NODE_STRUCT_VERSION;
  syn.mime_color_rgba = 0x47474DFFu;
  {
    double shadow =
        style->gradient_strength * (DM_TREEMAP_DEFAULT_SHADOW_STRENGTH / DM_TREEMAP_DEFAULT_GRADIENT_STRENGTH);
    dm_file_node_compute_gradient_colors(&syn, style->gradient_strength, shadow);
  }

  dm_treemap_draw_gradient_tile(cr, &syn, x, y, width, height, style);
}
